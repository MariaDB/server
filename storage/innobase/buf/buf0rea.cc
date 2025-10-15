/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
#include "page0zip.h"
#include "log0recv.h"
#include "trx0sys.h"
#include "os0file.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "log.h"

TRANSACTIONAL_TARGET
bool buf_pool_t::page_hash_contains(const page_id_t page_id, hash_chain &chain)
	noexcept
{
  transactional_shared_lock_guard<page_hash_latch> g
    {page_hash.lock_get(chain)};
  return page_hash.get(page_id, chain);
}

/** If there are buf_pool.curr_size() per the number below pending reads, then
read-ahead is not done: this is to prevent flooding the buffer pool with
i/o-fixed buffer blocks */
#define BUF_READ_AHEAD_PEND_LIMIT	2

/** Initialize a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if the tablespace has been or is being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later.
@param page_id    page identifier
@param zip_size   ROW_FORMAT=COMPRESSED page size, or 0,
                  bitwise-ORed with 1 in recovery
@param chain      buf_pool.page_hash cell for page_id
@param block      preallocated buffer block (set to nullptr if consumed)
@return pointer to the block
@retval	nullptr in case of an error */
static buf_page_t *buf_page_init_for_read(const page_id_t page_id,
                                          ulint zip_size,
                                          buf_pool_t::hash_chain &chain,
                                          buf_block_t *&block) noexcept
{
  buf_page_t *bpage= !zip_size || (zip_size & 1) ? &block->page : nullptr;
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  hash_lock.lock();
  if (buf_pool.page_hash.get(page_id, chain))
  {
page_exists:
    hash_lock.unlock();
    /* The page is already in the buffer pool. */
    return nullptr;
  }

  if (UNIV_UNLIKELY(mysql_mutex_trylock(&buf_pool.mutex)))
  {
    hash_lock.unlock();
    mysql_mutex_lock(&buf_pool.mutex);
    hash_lock.lock();
    if (buf_pool.page_hash.get(page_id, chain))
    {
      mysql_mutex_unlock(&buf_pool.mutex);
      goto page_exists;
    }
  }

  zip_size&= ~1;

  if (UNIV_LIKELY(bpage != nullptr))
  {
    block= nullptr;
    reinterpret_cast<buf_block_t*>(bpage)->
      initialise(page_id, zip_size & ~1, buf_page_t::READ_FIX);
    /* x_unlock() will be invoked
    in buf_page_t::read_complete() by the io-handler thread. */
    bpage->lock.x_lock(true);
    /* Insert into the hash table of file pages */
    buf_pool.page_hash.append(chain, bpage);
    hash_lock.unlock();

    /* The block must be put to the LRU list, to the old blocks */
    buf_LRU_add_block(bpage, true/* to old blocks */);

    if (UNIV_UNLIKELY(zip_size))
    {
      /* buf_pool.mutex may be released and reacquired by
      buf_buddy_alloc(). We must defer this operation until after the
      block descriptor has been added to buf_pool.LRU and
      buf_pool.page_hash. */
      bpage->zip.data= static_cast<page_zip_t*>(buf_buddy_alloc(zip_size));

      /* To maintain the invariant
      block->in_unzip_LRU_list == block->page.belongs_to_unzip_LRU()
      we have to add this block to unzip_LRU
      after block->page.zip.data is set. */
      ut_ad(bpage->belongs_to_unzip_LRU());
      buf_unzip_LRU_add_block(reinterpret_cast<buf_block_t*>(bpage), TRUE);
    }
  }
  else
  {
    hash_lock.unlock();
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
      if (UNIV_LIKELY_NULL(buf_pool.page_hash.get(page_id, chain)))
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

    /* Because bpage is a compressed-only block descriptor, it cannot be
    passed to buf_pool.page_guess(), and therefore there is no risk of a
    a false match. Therefore, we can safely initialize bpage before
    acquiring hash_lock. */
    bpage->lock.init();
    bpage->init(buf_page_t::READ_FIX, page_id);
    bpage->lock.x_lock(true);

    hash_lock.lock();
    buf_pool.page_hash.append(chain, bpage);
    hash_lock.unlock();

    /* The block must be put to the LRU list, to the old blocks.
    The zip size is already set into the page zip */
    buf_LRU_add_block(bpage, true/* to old blocks */);
  }

  buf_pool.stat.n_pages_read++;
func_exit:
  mysql_mutex_unlock(&buf_pool.mutex);
  ut_ad(!bpage || bpage->in_file());

  return bpage;
}

inline ulonglong mariadb_measure() noexcept
{
#if (MY_TIMER_ROUTINE_CYCLES)
  return my_timer_cycles();
#else
  return my_timer_microseconds();
#endif
}

/** Low-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there, in which case does nothing.
Sets the io_fix flag and sets an exclusive lock on the buffer frame. The
flag is cleared and the x-lock released by an i/o-handler thread.

@param[in] page_id	page id
@param[in] zip_size	0 or ROW_FORMAT=COMPRESSED page size
			bitwise-ORed with 1 to allocate an uncompressed frame
@param[in,out] chain	buf_pool.page_hash cell for page_id
@param[in,out] space	tablespace
@param[in,out] block	preallocated buffer block
@param[in] thd		current_thd if sync
@param[in] sync		whether synchronous aio is desired
@return error code
@retval DB_SUCCESS if the page was read
@retval DB_SUCCESS_LOCKED_REC if the page exists in the buffer pool already */
static
dberr_t
buf_read_page_low(
	const page_id_t		page_id,
	ulint			zip_size,
	buf_pool_t::hash_chain&	chain,
	fil_space_t*		space,
	buf_block_t*&		block,
	THD*			thd = nullptr,
	bool			sync = false) noexcept
{
	buf_page_t*	bpage;

	if (buf_dblwr.is_inside(page_id)) {
		space->release();
		return DB_PAGE_CORRUPTED;
	}

	bpage = buf_page_init_for_read(page_id, zip_size, chain, block);

	if (!bpage) {
		space->release();
		return DB_SUCCESS_LOCKED_REC;
	}

	ut_ad(bpage->in_file());

	DBUG_LOG("ib_buf",
		 "read page " << page_id << " zip_size=" << zip_size
		 << (sync ? " sync" : " async"));

	void* dst = zip_size > 1 ? bpage->zip.data : bpage->frame;
	const ulint len = zip_size & ~1 ? zip_size & ~1 : srv_page_size;

	fil_io_t fio;

	if (sync) {
		ulonglong mariadb_timer = 0;
		trx_t *const trx= thd ? thd_to_trx(thd) : nullptr;
		if (trx && trx->active_handler_stats) {
			mariadb_timer = mariadb_measure();
		}

		thd_wait_begin(thd, THD_WAIT_DISKIO);
		fio = space->io(IORequest(IORequest::READ_SYNC),
				os_offset_t{page_id.page_no()} * len, len,
				dst, bpage);
		thd_wait_end(thd);
		if (mariadb_timer) {
			trx->active_handler_stats->pages_read_time
				+= mariadb_measure() - mariadb_timer;
		}
		if (UNIV_UNLIKELY(fio.err != DB_SUCCESS)) {
			goto read_fail;
		}
		/* The i/o was already completed in space->io() */
		fio.err = bpage->read_complete(*fio.node);
		space->release();
	} else {
		fio = space->io(IORequest(IORequest::READ_ASYNC),
				os_offset_t{page_id.page_no()} * len, len,
				dst, bpage);
		if (UNIV_UNLIKELY(fio.err != DB_SUCCESS)) {
		read_fail:
			recv_sys.free_corrupted_page(page_id,
						     *space->chain.start);
			buf_pool.corrupted_evict(bpage, buf_page_t::READ_FIX);
		}
	}

	return fio.err;
}

/** Acquire a buffer block. */
static buf_block_t *buf_read_acquire()
{
  return buf_LRU_get_free_block(have_no_mutex_soft);
}

/** Free a buffer block if needed. */
static void buf_read_release(buf_block_t *block)
{
  if (block)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    buf_LRU_block_free_non_file_page(block);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
}

/** Report a completed read-ahead batch.
@param space  tablespace
@param count  number of pages submitted for reading */
static ATTRIBUTE_NOINLINE
void buf_read_ahead_report(const fil_space_t &space, size_t count) noexcept
{
  if (THD *thd= current_thd)
    if (trx_t *trx= thd_to_trx(thd))
      if (ha_handler_stats *stats= trx->active_handler_stats)
        stats->pages_prefetched+= count;
  mysql_mutex_lock(&buf_pool.mutex);
  /* Read ahead is considered one I/O operation for the purpose of
     LRU policy decision. */
  buf_LRU_stat_inc_io();
  buf_pool.stat.n_ra_pages_read_rnd+= count;
  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Applies a random read-ahead in buf_pool if there are at least a threshold
value of accessed pages from the random read-ahead area. Does not read any
page, not even the one at the position (space, offset), if the read-ahead
mechanism is not activated. NOTE: the calling thread may own latches on
pages: to avoid deadlocks this function must be written such that it cannot
end up waiting for these latches!
@param[in]	page_id		page id of a page which the current thread
wants to access
@return number of page read requests issued */
TRANSACTIONAL_TARGET
ulint buf_read_ahead_random(const page_id_t page_id) noexcept
{
  if (!srv_random_read_ahead || page_id.space() >= SRV_TMP_SPACE_ID)
    /* Disable the read-ahead for temporary tablespace */
    return 0;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return 0;

  if (os_aio_pending_reads_approx() >
      buf_pool.curr_size() / BUF_READ_AHEAD_PEND_LIMIT)
    return 0;

  fil_space_t* space= fil_space_t::get(page_id.space());
  if (!space)
    return 0;

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

no_read_ahead:
  space->release();
  return 0;

read_ahead:
  if (space->is_stopping())
    goto no_read_ahead;

  /* Read all the suitable blocks within the area */
  buf_block_t *block= nullptr;
  unsigned zip_size{space->zip_size()};
  if (UNIV_LIKELY(!zip_size))
  {
  allocate_block:
    if (UNIV_UNLIKELY(!(block= buf_read_acquire())))
      goto no_read_ahead;
  }
  else if (recv_recovery_is_on())
  {
    zip_size|= 1;
    goto allocate_block;
  }

  for (page_id_t i= low; i < high; ++i)
  {
    if (space->is_stopping())
      break;
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
    space->reacquire();
    if (buf_read_page_low(i, zip_size, chain, space, block) == DB_SUCCESS)
    {
      count++;
      ut_ad(!block);
      if ((UNIV_LIKELY(!zip_size) || (zip_size & 1)) &&
          UNIV_UNLIKELY(!(block= buf_read_acquire())))
        break;
    }
  }

  if (count)
    buf_read_ahead_report(*space, count);

  space->release();
  buf_read_release(block);
  return count;
}

dberr_t buf_read_page(const page_id_t page_id,
                      buf_pool_t::hash_chain &chain, bool unzip) noexcept
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

  /* Our caller should already have ensured that the page does not
  exist in buf_pool.page_hash. */
  buf_block_t *block= nullptr;
  unsigned zip_size= space->zip_size();

  if (UNIV_LIKELY(!zip_size))
  {
  allocate_block:
    mysql_mutex_lock(&buf_pool.mutex);
    buf_LRU_stat_inc_io();
    block= buf_LRU_get_free_block(have_mutex);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
  else if (unzip)
  {
    zip_size|= 1;
    goto allocate_block;
  }

  dberr_t err= buf_read_page_low(page_id, zip_size, chain, space, block,
                                 current_thd, true);
  buf_read_release(block);
  return err;
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
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  if (buf_pool.page_hash_contains(page_id, chain))
  {
  skip:
    space->release();
    return;
  }

  buf_block_t *block= nullptr;
  if (UNIV_LIKELY(!zip_size))
  {
  allocate_block:
    if (UNIV_UNLIKELY(!(block= buf_read_acquire())))
      goto skip;
  }
  else if (recv_recovery_is_on())
  {
    zip_size|= 1;
    goto allocate_block;
  }

  if (buf_read_page_low(page_id, zip_size, chain, space, block) ==
      DB_SUCCESS)
    ut_ad(!block);
  else
    buf_read_release(block);

  /* We do not increment number of I/O operations used for LRU policy
  here (buf_LRU_stat_inc_io()). We use this in heuristics to decide
  about evicting uncompressed version of ROW_FORMAT=COMPRESSED pages
  from the buffer pool. Since this function is called from buffer pool
  load these IOs are deliberate and are not part of normal workload we
  can ignore these in our heuristics. */
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
@param[in]	page_id		page id; see NOTE 3 above
@return number of page read requests issued */
ulint buf_read_ahead_linear(const page_id_t page_id) noexcept
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

  if (high_1.page_no() > space->last_page_number())
  {
    /* The area is not whole. */
fail:
    space->release();
    return 0;
  }

  if (trx_sys_hdr_page(page_id))
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
    if (!bpage)
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
  buf_block_t *block= nullptr;
  unsigned zip_size{space->zip_size()};
  if (UNIV_LIKELY(!zip_size))
  {
  allocate_block:
    if (UNIV_UNLIKELY(!(block= buf_read_acquire())))
      goto fail;
  }
  else if (recv_recovery_is_on())
  {
    zip_size|= 1;
    goto allocate_block;
  }

  count= 0;
  for (; new_low <= new_high_1; ++new_low)
  {
    if (space->is_stopping())
      break;
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(new_low.fold());
    space->reacquire();
    if (buf_read_page_low(new_low, zip_size, chain, space, block) ==
        DB_SUCCESS)
    {
      count++;
      ut_ad(!block);
      if ((UNIV_LIKELY(!zip_size) || (zip_size & 1)) &&
          UNIV_UNLIKELY(!(block= buf_read_acquire())))
        break;
    }
  }

  if (count)
    buf_read_ahead_report(*space, count);

  space->release();
  buf_read_release(block);
  return count;
}

/** Schedule a page for recovery.
@param space    tablespace
@param page_id  page identifier
@param recs     log records
@param init_lsn page initialization, or 0 if the page needs to be read */
void buf_read_recover(fil_space_t *space, const page_id_t page_id,
                      page_recv_t &recs, lsn_t init_lsn) noexcept
{
  ut_ad(space->id == page_id.space());
  space->reacquire();
  const ulint zip_size= space->zip_size() | 1;
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  buf_block_t *block= buf_LRU_get_free_block(have_no_mutex);

  if (init_lsn)
  {
    if (buf_page_t *bpage=
        buf_page_init_for_read(page_id, zip_size, chain, block))
    {
      ut_ad(bpage->in_file());
      os_fake_read(IORequest{bpage, (buf_tmp_buffer_t*) &recs,
                             UT_LIST_GET_FIRST(space->chain),
                             IORequest::READ_ASYNC}, init_lsn);
      return;
    }
  }
  else if (dberr_t err=
           buf_read_page_low(page_id, zip_size, chain, space, block))
  {
    if (err != DB_SUCCESS_LOCKED_REC)
      sql_print_error("InnoDB: Recovery failed to read page "
                      UINT32PF " from %s",
                      page_id.page_no(), space->chain.start->name);
  }
  else
  {
    ut_ad(!block);
    return;
  }

  buf_LRU_block_free_non_file_page(block);
}
