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
@retval nullptr in case of an error
@retval pointer to block | 1 if the page already exists in buf_pool */
static buf_page_t *buf_page_init_for_read(const page_id_t page_id,
                                          ulint zip_size,
                                          buf_pool_t::hash_chain &chain,
                                          buf_block_t *&block) noexcept
{
  buf_page_t *bpage= !zip_size || (zip_size & 1) ? &block->page : nullptr;
  constexpr uint32_t READ_BUF_FIX{buf_page_t::READ_FIX + 1};
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  hash_lock.lock();
  buf_page_t *hash_page= buf_pool.page_hash.get(page_id, chain);
  if (hash_page)
  {
  page_exists:
    /* The page is already in the buffer pool. */
    ut_d(const uint32_t state=) hash_page->fix();
    ut_ad(state >= buf_page_t::FREED);
    hash_lock.unlock();
    return reinterpret_cast<buf_page_t*>(uintptr_t(hash_page) | 1);
  }

  zip_size&= ~1;
  uint16_t ssize= 0;
  if (zip_size)
  {
    for (ssize= 1; zip_size > (512U << ssize); ssize++) {}
    ut_ad(ssize < 1U << PAGE_ZIP_SSIZE_BITS);
    ut_ad(zip_size == 512U << ssize);
  }

  if (UNIV_UNLIKELY(mysql_mutex_trylock(&buf_pool.mutex)))
  {
    hash_lock.unlock();
    mysql_mutex_lock(&buf_pool.mutex);
    hash_lock.lock();
    hash_page= buf_pool.page_hash.get(page_id, chain);
    if (hash_page)
    {
      mysql_mutex_unlock(&buf_pool.mutex);
      goto page_exists;
    }
  }

  if (UNIV_LIKELY(bpage != nullptr))
  {
    block= nullptr;
    reinterpret_cast<buf_block_t*>(bpage)->
      initialise(page_id, ssize, READ_BUF_FIX);
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
    ut_ad(ut_is_2pow(zip_size));

    /* The ROW_FORMAT=COMPRESSED page must be allocated before the
    block descriptor bpage in order to avoid the invocation of
    buf_buddy_relocate_block() on uninitialized data. */
    bool lru= false;
    void *data= buf_buddy_alloc(zip_size, &lru);

    /* If buf_buddy_alloc() allocated storage from the LRU list,
    it released and reacquired buf_pool.mutex.  Thus, we must
    check the page_hash again, as it may have been modified. */
    if (UNIV_UNLIKELY(lru))
    {
      hash_page= buf_pool.page_hash.get(page_id, chain);
      if (UNIV_LIKELY_NULL(hash_page))
      {
        /* The block was added by some other thread. */
        ut_d(const uint32_t state=) hash_page->fix();
        ut_ad(state >= buf_page_t::FREED);
        buf_buddy_free(data, zip_size);
        mysql_mutex_unlock(&buf_pool.mutex);
        return reinterpret_cast<buf_page_t*>(uintptr_t(hash_page) | 1);
      }
    }

    bpage= static_cast<buf_page_t*>(ut_zalloc_nokey(sizeof *bpage));

    /* Because bpage is a compressed-only block descriptor, it cannot be
    passed to buf_pool.page_guess(), and therefore there is no risk of a
    a false match. Therefore, we can safely initialize bpage before
    acquiring hash_lock. */
    bpage->lock.init();
    bpage->init(READ_BUF_FIX, page_id, ssize);
    bpage->zip.data= static_cast<page_zip_t*>(data);
    bpage->lock.x_lock(true);

    hash_lock.lock();
    buf_pool.page_hash.append(chain, bpage);
    hash_lock.unlock();

    /* The block must be put to the LRU list, to the old blocks.
    The zip size is already set into the page zip */
    buf_LRU_add_block(bpage, true/* to old blocks */);
  }

  buf_pool.stat.n_pages_read++;
  ut_ad(!bpage || bpage->in_file());
  mysql_mutex_unlock(&buf_pool.mutex);
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

void buf_page_t::read_wait(trx_t *trx) noexcept
{
  ulonglong start= 0, *stats= nullptr;
  if (trx)
  {
    tpool::tpool_wait_begin();
    thd_wait_begin(trx->mysql_thd, THD_WAIT_DISKIO);
    if (ha_handler_stats *active= trx->active_handler_stats)
    {
      active->pages_read_count++;
      stats= &active->pages_read_time;
      start= mariadb_measure();
    }
  }
  lock.s_lock_nospin();
  ut_d(const uint32_t latched_state{state()});
  ut_ad(latched_state > FREED);
  ut_ad(latched_state < READ_FIX || latched_state > WRITE_FIX);
  if (trx)
  {
    tpool::tpool_wait_end();
    thd_wait_end(trx->mysql_thd);
    if (stats)
      *stats+= mariadb_measure() - start;
  }
}

/** Low-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there, in which case does nothing.
Sets the io_fix flag and sets an exclusive lock on the buffer frame. The
flag is cleared and the x-lock released by an i/o-handler thread.

@param[in] page_id	page id
@param[in] zip_size	0 or ROW_FORMAT=COMPRESSED page size
			bitwise-ORed with 1 to allocate an uncompressed frame
@param[out] err		nullptr for asynchronous; error code for synchronous:
			DB_SUCCESS if the page was successfully read,
			DB_SUCCESS_LOCKED_REC if the exists in the pool,
			DB_PAGE_CORRUPTED on page checksum mismatch,
			DB_DECRYPTION_FAILED if page post encryption checksum
			matches but after decryption normal page checksum
			does not match
@param[in,out] chain	buf_pool.page_hash cell for page_id
@param[in,out] space	tablespace
@param[in,out] block	preallocated buffer block
@param[in] thd		current_thd if sync
@return buffer-fixed block (*err may be set to DB_SUCCESS_LOCKED_REC)
@retval -1 if err==nullptr and an asynchronous read was submitted
@retval -2 if err==nullptr and the page exists in the buffer pool
@retval nullptr if the page was not successfully read (*err will be set) */
static
buf_page_t*
buf_read_page_low(
	const page_id_t		page_id,
	ulint			zip_size,
	dberr_t*		err,
	buf_pool_t::hash_chain&	chain,
	fil_space_t*		space,
	buf_block_t*&		block,
	THD*			thd = nullptr) noexcept
{
  if (buf_dblwr.is_inside(page_id))
  {
  fail:
    space->release();
    if (err)
      *err= DB_PAGE_CORRUPTED;
    return nullptr;
  }

  buf_page_t *bpage= buf_page_init_for_read(page_id, zip_size, chain, block);
  if (UNIV_UNLIKELY(!bpage))
    goto fail;
  const bool exist(uintptr_t(bpage) & 1);
  bpage= reinterpret_cast<buf_page_t*>(uintptr_t(bpage) & ~uintptr_t{1});
  trx_t *const trx= thd ? thd_to_trx(thd) : nullptr;
  if (exist)
  {
    if (!err)
    {
      bpage->unfix();
      bpage= reinterpret_cast<buf_page_t*>(-2);
    }
    else
    {
      uint32_t state{bpage->state()};
      ut_d(page_id_t id{bpage->id()});
      ut_ad(state > buf_page_t::FREED);
      if (state < buf_page_t::UNFIXED)
      {
      corrupted:
        ut_ad(id == page_id || id == page_id_t{~0ULL});
        bpage->unfix();
        bpage= nullptr;
        *err= DB_PAGE_CORRUPTED;
      }
      else if (!bpage->is_read_fixed(state))
        *err= DB_SUCCESS_LOCKED_REC;
      else
      {
        bpage->read_wait(trx);
        state= bpage->state();
        ut_d(id= bpage->id());
        bpage->lock.s_unlock();
        if (state < buf_page_t::UNFIXED)
          goto corrupted;
      }
    }

    space->release();
    return bpage;
  }

  ut_ad(bpage->in_file());

  void* dst= zip_size > 1 ? bpage->zip.data : bpage->frame;
  const size_t len= zip_size & ~1 ? zip_size & ~1 : srv_page_size;

  if (err != nullptr)
  {
    thd_wait_begin(thd, THD_WAIT_DISKIO);
    ha_handler_stats *const stats= trx ? trx->active_handler_stats : nullptr;
    const ulonglong start= stats ? mariadb_measure() : 0;
    auto fio= space->io(IORequest(IORequest::READ_SYNC),
                        os_offset_t{page_id.page_no()} * len, len, dst, bpage);
    *err= fio.err;
    thd_wait_end(thd);
    if (stats)
    {
      stats->pages_read_count++;
      if (start)
        stats->pages_read_time+= mariadb_measure() - start;
    }
    if (UNIV_LIKELY(*err == DB_SUCCESS))
    {
      *err= bpage->read_complete(*fio.node, recv_sys.recovery_on);
      if (*err)
        bpage= nullptr;
      space->release();

      /* FIXME: Remove this, and accumulate stats->pages_read_count to
      global statistics somewhere! */
      buf_LRU_stat_inc_io();
      return bpage;
    }
  }
  else if (UNIV_LIKELY(DB_SUCCESS ==
                       space->io(IORequest(IORequest::READ_ASYNC),
                                 os_offset_t{page_id.page_no()} * len, len,
                                 dst, bpage).err))
    return reinterpret_cast<buf_page_t*>(-1);

  recv_sys.free_corrupted_page(page_id, *space->chain.start);
  buf_pool.corrupted_evict(bpage, buf_page_t::READ_FIX + 1);
  return nullptr;
}

/** Acquire a buffer block. */
static buf_block_t *buf_read_acquire()
{
  return buf_LRU_get_free_block(have_no_mutex_soft);
}

/** Free a buffer block if needed.
@param block  block to be freed */
static void buf_read_release(buf_block_t *block) noexcept
{
  if (block)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    buf_LRU_block_free_non_file_page(block);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
}

ATTRIBUTE_NOINLINE
/** Free a buffer block if needed, and update the read-ahead count.
@param block  block to be freed
@param count  number of blocks that were read ahead
@return count*/
static size_t buf_read_release_count(buf_block_t *block, size_t count) noexcept
{
  if (block || count)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    if (block)
      buf_LRU_block_free_non_file_page(block);
    if (count)
    {
      /* Read ahead is considered one I/O operation for the purpose of
      LRU policy decision. */
      buf_LRU_stat_inc_io();
      buf_pool.stat.n_ra_pages_read+= count;
    }
    mysql_mutex_unlock(&buf_pool.mutex);
  }

  if (count)
    if (THD *thd= current_thd)
      if (trx_t *trx= thd_to_trx(thd))
        if (ha_handler_stats *stats= trx->active_handler_stats)
          stats->pages_prefetched+= count;
  return count;
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
    {
      const auto state= bpage->zip.get_state();
      if ((bpage->zip.is_accessed(state) ||
           (!bpage->zip.old(state) && bpage->is_accessed())) &&
          !--count)
        goto read_ahead;
    }
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

  /* Read all the suitable blocks within the area */
  for (page_id_t i= low; i < high; ++i)
  {
    if (space->is_stopping())
      break;
    buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
    space->reacquire();
    if (reinterpret_cast<buf_page_t*>(-1) ==
        buf_read_page_low(i, zip_size, nullptr, chain, space, block, nullptr))
    {
      count++;
      ut_ad(!block);
      if ((UNIV_LIKELY(!zip_size) || (zip_size & 1)) &&
          UNIV_UNLIKELY(!(block= buf_read_acquire())))
        break;
    }
  }

  space->release();

  return buf_read_release_count(block, count);
}

buf_block_t *buf_read_page(const page_id_t page_id, dberr_t *err,
                           buf_pool_t::hash_chain &chain, bool unzip) noexcept
{
  fil_space_t *space= fil_space_t::get(page_id.space());
  dberr_t local_err;
  if (!err)
    err= &local_err;
  if (UNIV_UNLIKELY(!space))
  {
    sql_print_information("InnoDB: trying to read page "
                          "[page id: space=" UINT32PF
                          ", page number=" UINT32PF "]"
                          " in nonexisting or being-dropped tablespace",
                          page_id.space(), page_id.page_no());
    *err= DB_TABLESPACE_DELETED;
    return nullptr;
  }

  /* Our caller should already have ensured that the page does not
  exist in buf_pool.page_hash. */
  buf_block_t *block= nullptr;
  unsigned zip_size= space->zip_size();

  if (UNIV_LIKELY(!zip_size))
  {
  allocate_block:
    mysql_mutex_lock(&buf_pool.mutex);
    block= buf_LRU_get_free_block(have_mutex);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
  else if (unzip)
  {
    zip_size|= 1;
    goto allocate_block;
  }

  buf_page_t *b= buf_read_page_low(page_id, zip_size, err, chain, space,
                                   block, current_thd);
  buf_read_release(block);
  return reinterpret_cast<buf_block_t*>(b);
}

void buf_read_page_background(const page_id_t page_id, fil_space_t *space,
                              trx_t *trx) noexcept
{
  ut_ad(!recv_recovery_is_on());
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  if (buf_pool.page_hash_contains(page_id, chain))
  skip:
    space->release();
  else
  {
    buf_block_t *b= nullptr;
    ulint zip_size{space->zip_size()};
    if (UNIV_LIKELY(!zip_size) && UNIV_UNLIKELY(!(b= buf_read_acquire())))
      goto skip;
    buf_read_page_low(page_id, zip_size, nullptr, chain, space, b, nullptr);
    if (b || trx)
    {
      mysql_mutex_lock(&buf_pool.mutex);
      if (b)
        buf_LRU_block_free_non_file_page(b);
      if (UNIV_LIKELY(trx != nullptr))
      {
        buf_LRU_stat_inc_io();
        buf_pool.stat.n_ra_pages_read++;
      }
      mysql_mutex_unlock(&buf_pool.mutex);
    }
    if (!trx);
    else if (ha_handler_stats *stats= trx->active_handler_stats)
      stats->pages_prefetched++;
    /* buf_load() invokes this with trx=nullptr. In that case, we will
    not update any statistics; these deliberate page reads are not
    part of a normal workload and therefore should not affect the
    unzip_LRU heuristics. */
  }
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
    if (reinterpret_cast<buf_page_t*>(-1) ==
        buf_read_page_low(new_low, zip_size, nullptr,
                          chain, space, block, nullptr))
    {
      count++;
      ut_ad(!block);
      if ((UNIV_LIKELY(!zip_size) || (zip_size & 1)) &&
          UNIV_UNLIKELY(!(block= buf_read_acquire())))
        break;
    }
  }

  space->release();
  return buf_read_release_count(block, count);
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
    buf_page_t *bpage= buf_page_init_for_read(page_id, zip_size, chain, block);
    if (UNIV_UNLIKELY(!bpage))
      goto fail;
    const bool exist(uintptr_t(bpage) & 1);
    bpage= reinterpret_cast<buf_page_t*>(uintptr_t(bpage) & ~uintptr_t{1});
    bpage->unfix();

    if (!exist)
      os_fake_read(IORequest{bpage, (buf_tmp_buffer_t*) &recs,
                             UT_LIST_GET_FIRST(space->chain),
                             IORequest::READ_ASYNC}, init_lsn);
  }
  else if (!buf_read_page_low(page_id, zip_size, nullptr, chain, space, block,
                              nullptr))
  fail:
    sql_print_error("InnoDB: Recovery failed to read page %" PRIu32 " from %s",
                    page_id.page_no(), space->chain.start->name);

  buf_read_release(block);
}
