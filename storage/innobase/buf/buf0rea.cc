/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0rea.cc
The database buffer read

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <mysql/service_thd_wait.h>

#include "buf0rea.h"
#include "fil0fil.h"
#include "mtr0mtr.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "buf0buddy.h"
#include "buf0dblwr.h"
#include "ibuf0ibuf.h"
#include "log0recv.h"
#include "trx0sys.h"
#include "os0file.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "log.h"
#include "mariadb_stats.h"

/** If there are buf_pool.curr_size() per the number below pending reads, then
read-ahead is not done: this is to prevent flooding the buffer pool with
i/o-fixed buffer blocks */
#define BUF_READ_AHEAD_PEND_LIMIT	2

/** Remove the sentinel block for the watch before replacing it with a
real block. watch_unset() or watch_occurred() will notice
that the block has been replaced with the real block.
@param w          sentinel
@param chain      locked hash table chain
@return           w->state() */
inline uint32_t buf_pool_t::watch_remove(buf_page_t *w,
                                         buf_pool_t::hash_chain &chain)
  noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(xtest() || page_hash.lock_get(chain).is_write_locked());
  ut_ad(w >= &watch[0]);
  ut_ad(w < &watch[array_elements(watch)]);
  ut_ad(!w->zip.data);

  uint32_t s{w->state()};
  w->set_state(buf_page_t::NOT_USED);
  ut_ad(s >= buf_page_t::UNFIXED);
  ut_ad(s < buf_page_t::READ_FIX);

  if (~buf_page_t::LRU_MASK & s)
    page_hash.remove(chain, w);

  ut_ad(!w->in_page_hash);
  w->id_= page_id_t(~0ULL);
  return s;
}

/** Initialize a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if we specify to read only ibuf pages and the page is not an ibuf page, or
(3) if the space is deleted or being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later.
@param[in]	mode			BUF_READ_IBUF_PAGES_ONLY, ...
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	unzip			whether the uncompressed page is
					requested (for ROW_FORMAT=COMPRESSED)
@return pointer to the block
@retval	NULL	in case of an error */
TRANSACTIONAL_TARGET
static buf_page_t* buf_page_init_for_read(ulint mode, const page_id_t page_id,
                                          ulint zip_size, bool unzip) noexcept
{
  mtr_t mtr;

  if (mode == BUF_READ_IBUF_PAGES_ONLY)
  {
    /* It is a read-ahead within an ibuf routine */
    ut_ad(!ibuf_bitmap_page(page_id, zip_size));
    ibuf_mtr_start(&mtr);

    if (!recv_no_ibuf_operations && !ibuf_page(page_id, zip_size, &mtr))
    {
      ibuf_mtr_commit(&mtr);
      return nullptr;
    }
  }
  else
    ut_ad(mode == BUF_READ_ANY_PAGE);

  buf_page_t *bpage= nullptr;
  buf_block_t *block= nullptr;
  if (!zip_size || unzip || recv_recovery_is_on())
  {
    block= buf_LRU_get_free_block(false);
    block->initialise(page_id, zip_size, buf_page_t::READ_FIX);
    /* x_unlock() will be invoked
    in buf_page_t::read_complete() by the io-handler thread. */
    block->page.lock.x_lock(true);
  }

  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());

  mysql_mutex_lock(&buf_pool.mutex);

  buf_page_t *hash_page= buf_pool.page_hash.get(page_id, chain);
  if (hash_page && !buf_pool.watch_is_sentinel(*hash_page))
  {
    /* The page is already in the buffer pool. */
    if (block)
    {
      block->page.lock.x_unlock(true);
      ut_d(block->page.set_state(buf_page_t::MEMORY));
      buf_LRU_block_free_non_file_page(block);
    }
    goto func_exit;
  }

  if (UNIV_LIKELY(block != nullptr))
  {
    bpage= &block->page;

    /* Insert into the hash table of file pages */
    if (hash_page)
    {
      transactional_lock_guard<page_hash_latch> g
        {buf_pool.page_hash.lock_get(chain)};
      bpage->set_state(buf_pool.watch_remove(hash_page, chain) +
                       (buf_page_t::READ_FIX - buf_page_t::UNFIXED));
      buf_pool.page_hash.append(chain, &block->page);
    }
    else
    {
      transactional_lock_guard<page_hash_latch> g
        {buf_pool.page_hash.lock_get(chain)};
      buf_pool.page_hash.append(chain, &block->page);
    }

    /* The block must be put to the LRU list, to the old blocks */
    buf_LRU_add_block(&block->page, true/* to old blocks */);

    if (UNIV_UNLIKELY(zip_size))
    {
      /* buf_pool.mutex may be released and reacquired by
      buf_buddy_alloc(). We must defer this operation until after the
      block descriptor has been added to buf_pool.LRU and
      buf_pool.page_hash. */
      block->page.zip.data= static_cast<page_zip_t*>
        (buf_buddy_alloc(zip_size));

      /* To maintain the invariant
      block->in_unzip_LRU_list == block->page.belongs_to_unzip_LRU()
      we have to add this block to unzip_LRU
      after block->page.zip.data is set. */
      ut_ad(block->page.belongs_to_unzip_LRU());
      buf_unzip_LRU_add_block(block, TRUE);
    }
  }
  else
  {
    /* The compressed page must be allocated before the
    control block (bpage), in order to avoid the
    invocation of buf_buddy_relocate_block() on
    uninitialized data. */
    bool lru= false;
    void *data= buf_buddy_alloc(zip_size, &lru);

    /* If buf_buddy_alloc() allocated storage from the LRU list,
    it released and reacquired buf_pool.mutex.  Thus, we must
    check the page_hash again, as it may have been modified. */
    if (UNIV_UNLIKELY(lru))
    {
      hash_page= buf_pool.page_hash.get(page_id, chain);

      if (UNIV_UNLIKELY(hash_page && !buf_pool.watch_is_sentinel(*hash_page)))
      {
        /* The block was added by some other thread. */
        buf_buddy_free(data, zip_size);
        goto func_exit;
      }
    }

    bpage= static_cast<buf_page_t*>(ut_zalloc_nokey(sizeof *bpage));

    page_zip_des_init(&bpage->zip);
    page_zip_set_size(&bpage->zip, zip_size);
    bpage->zip.data = (page_zip_t*) data;

    bpage->lock.init();
    bpage->init(buf_page_t::READ_FIX, page_id);
    bpage->lock.x_lock(true);

    {
      transactional_lock_guard<page_hash_latch> g
        {buf_pool.page_hash.lock_get(chain)};

      if (hash_page)
        bpage->set_state(buf_pool.watch_remove(hash_page, chain) +
                         (buf_page_t::READ_FIX - buf_page_t::UNFIXED));

      buf_pool.page_hash.append(chain, bpage);
    }

    /* The block must be put to the LRU list, to the old blocks.
    The zip size is already set into the page zip */
    buf_LRU_add_block(bpage, true/* to old blocks */);
  }

  buf_pool.stat.n_pages_read++;
func_exit:
  mysql_mutex_unlock(&buf_pool.mutex);

  if (mode == BUF_READ_IBUF_PAGES_ONLY)
    ibuf_mtr_commit(&mtr);

  ut_ad(!bpage || bpage->in_file());

  return bpage;
}

/** Low-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there, in which case does nothing.
Sets the io_fix flag and sets an exclusive lock on the buffer frame. The
flag is cleared and the x-lock released by an i/o-handler thread.

@param[in,out] space	tablespace
@param[in] sync		true if synchronous aio is desired
@param[in] mode		BUF_READ_IBUF_PAGES_ONLY, ...,
@param[in] page_id	page id
@param[in] zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in] unzip	true=request uncompressed page
@return error code
@retval DB_SUCCESS if the page was read
@retval DB_SUCCESS_LOCKED_REC if the page exists in the buffer pool already */
static
dberr_t
buf_read_page_low(
	fil_space_t*		space,
	bool			sync,
	ulint			mode,
	const page_id_t		page_id,
	ulint			zip_size,
	bool			unzip) noexcept
{
	buf_page_t*	bpage;

	if (buf_dblwr.is_inside(page_id)) {
		space->release();
		return DB_PAGE_CORRUPTED;
	}

	if (sync) {
	} else if (trx_sys_hdr_page(page_id)
		   || ibuf_bitmap_page(page_id, zip_size)
		   || (!recv_no_ibuf_operations
		       && ibuf_page(page_id, zip_size, nullptr))) {

		/* Trx sys header is so low in the latching order that we play
		safe and do not leave the i/o-completion to an asynchronous
		i/o-thread. Change buffer pages must always be read with
		synchronous i/o, to make sure they do not get involved in
		thread deadlocks. */
		sync = true;
	}

	/* The following call will also check if the tablespace does not exist
	or is being dropped; if we succeed in initing the page in the buffer
	pool for read, then DISCARD cannot proceed until the read has
	completed */
	bpage = buf_page_init_for_read(mode, page_id, zip_size, unzip);

	if (!bpage) {
		space->release();
		return DB_SUCCESS_LOCKED_REC;
	}

	ut_ad(bpage->in_file());
	void* dst = zip_size ? bpage->zip.data : bpage->frame;
	const ulint len = zip_size ? zip_size : srv_page_size;
	fil_io_t fio;

	if (sync) {
		ulonglong start_time = 0;
		THD* const thd = current_thd;
		thd_wait_begin(thd, THD_WAIT_DISKIO);
		ha_handler_stats *stats = mariadb_stats;
		if (stats && stats->active) {
			start_time = mariadb_measure();
		}
		fio = space->io(IORequest(IORequest::READ_SYNC),
				os_offset_t{page_id.page_no()} * len, len,
				dst, bpage);
		thd_wait_end(thd);
		if (start_time) {
			stats->pages_read_time
				+= (mariadb_measure() - start_time);
		}
		if (UNIV_LIKELY(fio.err == DB_SUCCESS)) {
			dberr_t err{bpage->read_complete(*fio.node)};
			space->release();
			return err;
		}
	} else {
		fio = space->io(IORequest(IORequest::READ_ASYNC),
				os_offset_t{page_id.page_no()} * len, len,
				dst, bpage);
	}

	if (UNIV_UNLIKELY(fio.err != DB_SUCCESS)) {
		recv_sys.free_corrupted_page(page_id, *space->chain.start);
		buf_pool.corrupted_evict(bpage, buf_page_t::READ_FIX);
	}

	return fio.err;
}

/** Applies a random read-ahead in buf_pool if there are at least a threshold
value of accessed pages from the random read-ahead area. Does not read any
page, not even the one at the position (space, offset), if the read-ahead
mechanism is not activated. NOTE 1: the calling thread may own latches on
pages: to avoid deadlocks this function must be written such that it cannot
end up waiting for these latches! NOTE 2: the calling thread must want
access to the page given: this rule is set to prevent unintended read-aheads
performed by ibuf routines, a situation which could result in a deadlock if
the OS does not support asynchronous i/o.
@param[in]	page_id		page id of a page which the current thread
wants to access
@param[in]	ibuf		whether we are inside ibuf routine
@return number of page read requests issued; NOTE that if we read ibuf
pages, it may happen that the page at the given page number does not
get read even if we return a positive value! */
TRANSACTIONAL_TARGET
ulint buf_read_ahead_random(const page_id_t page_id, bool ibuf) noexcept
{
  if (!srv_random_read_ahead || page_id.space() >= SRV_TMP_SPACE_ID)
    /* Disable the read-ahead for temporary tablespace */
    return 0;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return 0;

  if (trx_sys_hdr_page(page_id))
    return 0;

  if (os_aio_pending_reads_approx() >
      buf_pool.curr_size() / BUF_READ_AHEAD_PEND_LIMIT)
    return 0;

  fil_space_t* space= fil_space_t::get(page_id.space());
  if (!space)
    return 0;

  const unsigned zip_size{space->zip_size()};

  if (ibuf_bitmap_page(page_id, zip_size))
  {
    /* If it is a change buffer bitmap page, we do no
    read-ahead, as that could break the ibuf page access order */
  no_read_ahead:
    space->release();
    return 0;
  }

  const uint32_t buf_read_ahead_area= buf_pool.read_ahead_area;
  ulint count= 5 + buf_read_ahead_area / 8;
  const page_id_t low= page_id - (page_id.page_no() % buf_read_ahead_area);
  page_id_t high= low + buf_read_ahead_area;
  high.set_page_no(std::min(high.page_no(), space->last_page_number()));

  /* Count how many blocks in the area have been recently accessed,
  that is, reside near the start of the LRU list. */

  for (page_id_t i= low; i < high; ++i)
  {
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    if (const buf_page_t *bpage= buf_pool.page_hash.get(i, chain))
      if (bpage->is_accessed() && buf_page_peek_if_young(bpage) && !--count)
        goto read_ahead;
  }

  goto no_read_ahead;

read_ahead:
  if (space->is_stopping())
    goto no_read_ahead;

  /* Read all the suitable blocks within the area */
  const ulint ibuf_mode= ibuf ? BUF_READ_IBUF_PAGES_ONLY : BUF_READ_ANY_PAGE;

  for (page_id_t i= low; i < high; ++i)
  {
    if (ibuf_bitmap_page(i, zip_size))
      continue;
    if (space->is_stopping())
      break;
    space->reacquire();
    if (buf_read_page_low(space, false, ibuf_mode, i, zip_size, false) ==
        DB_SUCCESS)
      count++;
  }

  if (count)
  {
    mariadb_increment_pages_prefetched(count);
    DBUG_PRINT("ib_buf", ("random read-ahead %zu pages from %s: %u",
			  count, space->chain.start->name,
			  low.page_no()));
    mysql_mutex_lock(&buf_pool.mutex);
    /* Read ahead is considered one I/O operation for the purpose of
    LRU policy decision. */
    buf_LRU_stat_inc_io();
    buf_pool.stat.n_ra_pages_read_rnd+= count;
    mysql_mutex_unlock(&buf_pool.mutex);
  }

  space->release();
  return count;
}

dberr_t buf_read_page(const page_id_t page_id, bool unzip) noexcept
{
  fil_space_t *space= fil_space_t::get(page_id.space());
  if (UNIV_UNLIKELY(!space))
  {
    sql_print_information("InnoDB: trying to read page "
                          "[page id: space=" UINT32PF
                          ", page number=" UINT32PF "]"
                          " in nonexisting or being-dropped tablespace",
                          page_id.space(), page_id.page_no());
    return DB_TABLESPACE_DELETED;
  }

  buf_LRU_stat_inc_io(); /* NOT protected by buf_pool.mutex */
  return buf_read_page_low(space, true, BUF_READ_ANY_PAGE,
                           page_id, space->zip_size(), unzip);
}

/** High-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there. Sets the io_fix flag and sets
an exclusive lock on the buffer frame. The flag is cleared and the x-lock
released by the i/o-handler thread.
@param[in,out]	space		tablespace
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0 */
void buf_read_page_background(fil_space_t *space, const page_id_t page_id,
                              ulint zip_size) noexcept
{
	buf_read_page_low(space, false, BUF_READ_ANY_PAGE,
			  page_id, zip_size, false);

	/* We do not increment number of I/O operations used for LRU policy
	here (buf_LRU_stat_inc_io()). We use this in heuristics to decide
	about evicting uncompressed version of compressed pages from the
	buffer pool. Since this function is called from buffer pool load
	these IOs are deliberate and are not part of normal workload we can
	ignore these in our heuristics. */
}

/** Applies linear read-ahead if in the buf_pool the page is a border page of
a linear read-ahead area and all the pages in the area have been accessed.
Does not read any page if the read-ahead mechanism is not activated. Note
that the algorithm looks at the 'natural' adjacent successor and
predecessor of the page, which on the leaf level of a B-tree are the next
and previous page in the chain of leaves. To know these, the page specified
in (space, offset) must already be present in the buf_pool. Thus, the
natural way to use this function is to call it when a page in the buf_pool
is accessed the first time, calling this function just after it has been
bufferfixed.
NOTE 1: as this function looks at the natural predecessor and successor
fields on the page, what happens, if these are not initialized to any
sensible value? No problem, before applying read-ahead we check that the
area to read is within the span of the space, if not, read-ahead is not
applied. An uninitialized value may result in a useless read operation, but
only very improbably.
NOTE 2: the calling thread may own latches on pages: to avoid deadlocks this
function must be written such that it cannot end up waiting for these
latches!
NOTE 3: the calling thread must want access to the page given: this rule is
set to prevent unintended read-aheads performed by ibuf routines, a situation
which could result in a deadlock if the OS does not support asynchronous io.
@param[in]	page_id		page id; see NOTE 3 above
@param[in]	ibuf		whether if we are inside ibuf routine
@return number of page read requests issued */
TRANSACTIONAL_TARGET
ulint buf_read_ahead_linear(const page_id_t page_id, bool ibuf) noexcept
{
  /* check if readahead is disabled.
  Disable the read ahead logic for temporary tablespace */
  if (!srv_read_ahead_threshold || page_id.space() >= SRV_TMP_SPACE_ID)
    return 0;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return 0;

  if (os_aio_pending_reads_approx() >
      buf_pool.curr_size() / BUF_READ_AHEAD_PEND_LIMIT)
    return 0;

  const uint32_t buf_read_ahead_area= buf_pool.read_ahead_area;
  const page_id_t low= page_id - (page_id.page_no() % buf_read_ahead_area);
  const page_id_t high_1= low + (buf_read_ahead_area - 1);

  /* We will check that almost all pages in the area have been accessed
  in the desired order. */
  const bool descending= page_id != low;

  if (!descending && page_id != high_1)
    /* This is not a border page of the area */
    return 0;

  fil_space_t *space= fil_space_t::get(page_id.space());
  if (!space)
    return 0;

  const unsigned zip_size= space->zip_size();

  if (high_1.page_no() > space->last_page_number())
  {
    /* The area is not whole. */
fail:
    space->release();
    return 0;
  }

  if (ibuf_bitmap_page(page_id, zip_size) || trx_sys_hdr_page(page_id))
    /* If it is an ibuf bitmap page or trx sys hdr, we do no
    read-ahead, as that could break the ibuf page access order */
    goto fail;

  /* How many out of order accessed pages can we ignore
  when working out the access pattern for linear readahead */
  ulint count= std::min<ulint>(buf_pool_t::READ_AHEAD_PAGES -
                               srv_read_ahead_threshold,
                               uint32_t{buf_pool.read_ahead_area});
  page_id_t new_low= low, new_high_1= high_1;
  unsigned prev_accessed= 0;
  for (page_id_t i= low; i <= high_1; ++i)
  {
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
    page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
    /* It does not make sense to use transactional_lock_guard here,
    because we would have many complex conditions inside the memory
    transaction. */
    hash_lock.lock_shared();

    const buf_page_t* bpage= buf_pool.page_hash.get(i, chain);
    if (!bpage || buf_pool.watch_is_sentinel(*bpage))
    {
      hash_lock.unlock_shared();
      if (i == page_id)
        goto fail;
failed:
      if (--count)
        continue;
      goto fail;
    }
    const unsigned accessed= bpage->is_accessed();
    if (i == page_id)
    {
      /* Read the natural predecessor and successor page addresses from
      the page; NOTE that because the calling thread may have an x-latch
      on the page, we do not acquire an s-latch on the page, this is to
      prevent deadlocks. The hash_lock is only protecting the
      buf_pool.page_hash for page i, not the bpage contents itself. */
      const byte *f= bpage->frame ? bpage->frame : bpage->zip.data;
      uint32_t prev= mach_read_from_4(my_assume_aligned<4>(f + FIL_PAGE_PREV));
      uint32_t next= mach_read_from_4(my_assume_aligned<4>(f + FIL_PAGE_NEXT));
      hash_lock.unlock_shared();
      /* The underlying file page of this buffer pool page could actually
      be marked as freed, or a read of the page into the buffer pool might
      be in progress. We may read uninitialized data here.
      Suppress warnings of comparing uninitialized values. */
      MEM_MAKE_DEFINED(&prev, sizeof prev);
      MEM_MAKE_DEFINED(&next, sizeof next);
      if (prev == FIL_NULL || next == FIL_NULL)
        goto fail;
      page_id_t id= page_id;
      if (descending)
      {
        if (id == high_1)
          ++id;
        else if (next - 1 != page_id.page_no())
          goto fail;
        else
          id.set_page_no(prev);
      }
      else
      {
        if (prev + 1 != page_id.page_no())
          goto fail;
        id.set_page_no(next);
      }

      new_low= id - (id.page_no() % buf_read_ahead_area);
      new_high_1= new_low + (buf_read_ahead_area - 1);

      if (id != new_low && id != new_high_1)
        /* This is not a border page of the area: return */
        goto fail;
      if (new_high_1.page_no() > space->last_page_number())
        /* The area is not whole */
        goto fail;
    }
    else
      hash_lock.unlock_shared();

    if (!accessed)
      goto failed;
    /* Note that buf_page_t::is_accessed() returns the time of the
    first access. If some blocks of the extent existed in the buffer
    pool at the time of a linear access pattern, the first access
    times may be nonmonotonic, even though the latest access times
    were linear. The threshold (srv_read_ahead_factor) should help a
    little against this. */
    bool fail= prev_accessed &&
      (descending ? prev_accessed > accessed : prev_accessed < accessed);
    prev_accessed= accessed;
    if (fail)
      goto failed;
  }

  /* If we got this far, read-ahead can be sensible: do it */
  count= 0;
  for (ulint ibuf_mode= ibuf ? BUF_READ_IBUF_PAGES_ONLY : BUF_READ_ANY_PAGE;
       new_low <= new_high_1; ++new_low)
  {
    if (ibuf_bitmap_page(new_low, zip_size))
      continue;
    if (space->is_stopping())
      break;
    space->reacquire();
    if (buf_read_page_low(space, false, ibuf_mode, new_low, zip_size, false) ==
        DB_SUCCESS)
      count++;
  }

  if (count)
  {
    mariadb_increment_pages_prefetched(count);
    DBUG_PRINT("ib_buf", ("random read-ahead %zu pages from %s: %u",
                          count, space->chain.start->name,
                          new_low.page_no()));
    mysql_mutex_lock(&buf_pool.mutex);
    /* Read ahead is considered one I/O operation for the purpose of
    LRU policy decision. */
    buf_LRU_stat_inc_io();
    buf_pool.stat.n_ra_pages_read+= count;
    mysql_mutex_unlock(&buf_pool.mutex);
  }

  space->release();
  return count;
}

/** Schedule a page for recovery.
@param space    tablespace
@param page_id  page identifier
@param recs     log records
@param init     page initialization, or nullptr if the page needs to be read */
void buf_read_recover(fil_space_t *space, const page_id_t page_id,
                      page_recv_t &recs, recv_init *init) noexcept
{
  ut_ad(space->id == page_id.space());
  space->reacquire();
  const ulint zip_size= space->zip_size();

  if (init)
  {
    if (buf_page_t *bpage= buf_page_init_for_read(BUF_READ_ANY_PAGE, page_id,
                                                  zip_size, true))
    {
      ut_ad(bpage->in_file());
      os_fake_read(IORequest{bpage, (buf_tmp_buffer_t*) &recs,
                             UT_LIST_GET_FIRST(space->chain),
                             IORequest::READ_ASYNC}, ptrdiff_t(init));
    }
  }
  else if (dberr_t err= buf_read_page_low(space, false, BUF_READ_ANY_PAGE,
                                          page_id, zip_size, true))
  {
    if (err != DB_SUCCESS_LOCKED_REC)
      sql_print_error("InnoDB: Recovery failed to read page "
                      UINT32PF " from %s",
                      page_id.page_no(), space->chain.start->name);
  }
}
