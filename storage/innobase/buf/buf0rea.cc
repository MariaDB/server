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

/** If there are buf_pool.curr_size per the number below pending reads, then
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
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@return pointer to the block
@retval	NULL	in case of an error */
TRANSACTIONAL_TARGET
static buf_page_t* buf_page_init_for_read(const page_id_t page_id,
                                          ulint zip_size)
{
  buf_page_t *bpage= nullptr;
  buf_block_t *block= nullptr;
  if (!zip_size || recv_recovery_is_on())
  {
    block= buf_LRU_get_free_block(false);
    block->initialise(page_id, zip_size, buf_page_t::READ_FIX);
    /* x_unlock() will be invoked
    in buf_page_t::read_complete() by the io-handler thread. */
    block->page.lock.x_lock(true);
  }

  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());

  mysql_mutex_lock(&buf_pool.mutex);

  if (buf_pool.page_hash.get(page_id, chain))
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

    bpage->init(buf_page_t::READ_FIX, page_id);
    bpage->lock.x_lock(true);

    {
      transactional_lock_guard<page_hash_latch> g
        {buf_pool.page_hash.lock_get(chain)};
      buf_pool.page_hash.append(chain, bpage);
    }

    /* The block must be put to the LRU list, to the old blocks.
    The zip size is already set into the page zip */
    buf_LRU_add_block(bpage, true/* to old blocks */);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
  buf_pool.n_pend_reads++;
  return bpage;
func_exit:
  mysql_mutex_unlock(&buf_pool.mutex);
  ut_ad(!bpage || bpage->in_file());

  return bpage;
}

/** Low-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there, in which case does nothing.
Sets the io_fix flag and sets an exclusive lock on the buffer frame. The
flag is cleared and the x-lock released by an i/o-handler thread.

@param[out] err		DB_SUCCESS or DB_TABLESPACE_DELETED
			if we are trying
			to read from a non-existent tablespace
@param[in,out] space	tablespace
@param[in] sync		true if synchronous aio is desired
@param[in] page_id	page id
@param[in] zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return whether a read request was queued */
static
bool
buf_read_page_low(
	dberr_t*		err,
	fil_space_t*		space,
	bool			sync,
	const page_id_t		page_id,
	ulint			zip_size)
{
	buf_page_t*	bpage;

	*err = DB_SUCCESS;

	if (buf_dblwr.is_inside(page_id)) {
		ib::error() << "Trying to read doublewrite buffer page "
			<< page_id;
		ut_ad(0);
nothing_read:
		space->release();
		return false;
	}

	/* The following call will also check if the tablespace does not exist
	or is being dropped; if we succeed in initing the page in the buffer
	pool for read, then DISCARD cannot proceed until the read has
	completed */
	bpage = buf_page_init_for_read(page_id, zip_size);

	if (bpage == NULL) {
		goto nothing_read;
	}

	ut_ad(bpage->in_file());

	if (sync) {
		thd_wait_begin(nullptr, THD_WAIT_DISKIO);
	}

	DBUG_LOG("ib_buf",
		 "read page " << page_id << " zip_size=" << zip_size
		 << (sync ? " sync" : " async"));

	void* dst = zip_size ? bpage->zip.data : bpage->frame;
	const ulint len = zip_size ? zip_size : srv_page_size;

	auto fio = space->io(IORequest(sync
				       ? IORequest::READ_SYNC
				       : IORequest::READ_ASYNC),
			     page_id.page_no() * len, len, dst, bpage);
	*err = fio.err;

	if (UNIV_UNLIKELY(fio.err != DB_SUCCESS)) {
		ut_d(auto n=) buf_pool.n_pend_reads--;
		ut_ad(n > 0);
		buf_pool.corrupted_evict(bpage, buf_page_t::READ_FIX);
	} else if (sync) {
		thd_wait_end(NULL);
		/* The i/o was already completed in space->io() */
		*err = bpage->read_complete(*fio.node);
		space->release();
	}

	return true;
}

/** Applies a random read-ahead in buf_pool if there are at least a threshold
value of accessed pages from the random read-ahead area. Does not read any
page, not even the one at the position (space, offset), if the read-ahead
mechanism is not activated. NOTE: the calling thread may own latches on
pages: to avoid deadlocks this function must be written such that it cannot
end up waiting for these latches!
@param[in]	page_id		page id of a page which the current thread
wants to access
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return number of page read requests issued */
TRANSACTIONAL_TARGET
ulint buf_read_ahead_random(const page_id_t page_id, ulint zip_size)
{
  if (!srv_random_read_ahead)
    return 0;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return 0;

  if (buf_pool.n_pend_reads > buf_pool.curr_size / BUF_READ_AHEAD_PEND_LIMIT)
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

  for (page_id_t i= low; i < high; ++i)
  {
    if (space->is_stopping())
      break;
    dberr_t err;
    space->reacquire();
    if (buf_read_page_low(&err, space, false, i, zip_size))
      count++;
  }

  if (count)
    DBUG_PRINT("ib_buf", ("random read-ahead %zu pages from %s: %u",
			  count, space->chain.start->name,
			  low.page_no()));
  space->release();

  /* Read ahead is considered one I/O operation for the purpose of
  LRU policy decision. */
  buf_LRU_stat_inc_io();

  buf_pool.stat.n_ra_pages_read_rnd+= count;
  srv_stats.buf_pool_reads.add(count);
  return count;
}

/** High-level function which reads a page from a file to buf_pool
if it is not already there. Sets the io_fix and an exclusive lock
on the buffer frame. The flag is cleared and the x-lock
released by the i/o-handler thread.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@retval DB_SUCCESS if the page was read and is not corrupted,
@retval DB_PAGE_CORRUPTED if page based on checksum check is corrupted,
@retval DB_DECRYPTION_FAILED if page post encryption checksum matches but
after decryption normal page checksum does not match.
@retval DB_TABLESPACE_DELETED if tablespace .ibd file is missing */
dberr_t buf_read_page(const page_id_t page_id, ulint zip_size)
{
  fil_space_t *space= fil_space_t::get(page_id.space());
  if (!space)
  {
    ib::info() << "trying to read page " << page_id
               << " in nonexisting or being-dropped tablespace";
    return DB_TABLESPACE_DELETED;
  }

  dberr_t err;
  if (buf_read_page_low(&err, space, true, page_id, zip_size))
    srv_stats.buf_pool_reads.add(1);

  buf_LRU_stat_inc_io();
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
                              ulint zip_size)
{
	dberr_t		err;

	if (buf_read_page_low(&err, space, false, page_id, zip_size)) {
		srv_stats.buf_pool_reads.add(1);
	}

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
@param[in]	page_id		page id; see NOTE 3 above
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return number of page read requests issued */
TRANSACTIONAL_TARGET
ulint buf_read_ahead_linear(const page_id_t page_id, ulint zip_size)
{
  /* check if readahead is disabled */
  if (!srv_read_ahead_threshold)
    return 0;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return 0;

  if (buf_pool.n_pend_reads > buf_pool.curr_size / BUF_READ_AHEAD_PEND_LIMIT)
    return 0;

  const uint32_t buf_read_ahead_area= buf_pool.read_ahead_area;
  const page_id_t low= page_id - (page_id.page_no() % buf_read_ahead_area);
  const page_id_t high_1= low + (buf_read_ahead_area - 1);

  /* We will check that almost all pages in the area have been accessed
  in the desired order. */
  const bool descending= page_id == low;

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

  /* How many out of order accessed pages can we ignore
  when working out the access pattern for linear readahead */
  ulint count= std::min<ulint>(buf_pool_t::READ_AHEAD_PAGES -
                               srv_read_ahead_threshold,
                               uint32_t{buf_pool.read_ahead_area});
  page_id_t new_low= low, new_high_1= high_1;
  unsigned prev_accessed= 0;
  for (page_id_t i= low; i != high_1; ++i)
  {
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    const buf_page_t* bpage= buf_pool.page_hash.get(i, chain);
    if (!bpage)
    {
      if (i == page_id)
        goto fail;
failed:
      if (--count)
        continue;
      goto fail;
    }
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
      if (prev == FIL_NULL || next == FIL_NULL)
        goto fail;
      page_id_t id= page_id;
      if (descending && next - 1 == page_id.page_no())
        id.set_page_no(prev);
      else if (!descending && prev + 1 == page_id.page_no())
        id.set_page_no(next);
      else
        goto fail; /* Successor or predecessor not in the right order */

      new_low= id - (id.page_no() % buf_read_ahead_area);
      new_high_1= new_low + (buf_read_ahead_area - 1);

      if (id != new_low && id != new_high_1)
        /* This is not a border page of the area: return */
        goto fail;
      if (new_high_1.page_no() > space->last_page_number())
        /* The area is not whole */
        goto fail;
    }

    const unsigned accessed= bpage->is_accessed();
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
  for (; new_low != new_high_1; ++new_low)
  {
    if (space->is_stopping())
      break;
    dberr_t err;
    space->reacquire();
    count+= buf_read_page_low(&err, space, false, new_low, zip_size);
  }

  if (count)
    DBUG_PRINT("ib_buf", ("random read-ahead %zu pages from %s: %u",
                          count, space->chain.start->name,
                          new_low.page_no()));
  space->release();

  /* Read ahead is considered one I/O operation for the purpose of
  LRU policy decision. */
  buf_LRU_stat_inc_io();

  buf_pool.stat.n_ra_pages_read+= count;
  return count;
}

/** @return whether a page has been freed */
inline bool fil_space_t::is_freed(uint32_t page)
{
  std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
  return freed_ranges.contains(page);
}

/** Issues read requests for pages which recovery wants to read in.
@param space_id	tablespace identifier
@param page_nos	page numbers to read, in ascending order */
void buf_read_recv_pages(uint32_t space_id, st_::span<uint32_t> page_nos)
{
	fil_space_t* space = fil_space_t::get(space_id);

	if (!space) {
		/* The tablespace is missing or unreadable: do nothing */
		return;
	}

	const ulint zip_size = space->zip_size();

	for (ulint i = 0; i < page_nos.size(); i++) {

		/* Ignore if the page already present in freed ranges. */
		if (space->is_freed(page_nos[i])) {
			continue;
		}

		const page_id_t	cur_page_id(space_id, page_nos[i]);

		ulint limit = 0;
		for (ulint j = 0; j < buf_pool.n_chunks; j++) {
			limit += buf_pool.chunks[j].size / 2;
		}

		for (ulint count = 0; buf_pool.n_pend_reads >= limit; ) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(10));

			if (!(++count % 1000)) {

				ib::error()
					<< "Waited for " << count / 100
					<< " seconds for "
					<< buf_pool.n_pend_reads
					<< " pending reads";
			}
		}

		dberr_t err;
		space->reacquire();
		buf_read_page_low(&err, space, false, cur_page_id, zip_size);

		if (err != DB_SUCCESS) {
			sql_print_error("InnoDB: Recovery failed to read page "
					UINT32PF " from %s",
					cur_page_id.page_no(),
					space->chain.start->name);
		}
	}


        DBUG_PRINT("ib_buf", ("recovery read (%zu pages) for %s",
			      page_nos.size(), space->chain.start->name));
	space->release();
}
