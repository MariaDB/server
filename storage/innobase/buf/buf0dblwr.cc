/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

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
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "buf0dblwr.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"

using st_::span;

/** The doublewrite buffer */
buf_dblwr_t buf_dblwr;

/** @return the TRX_SYS page */
inline buf_block_t *buf_dblwr_trx_sys_get(mtr_t *mtr)
{
  return buf_page_get(page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
                      0, RW_X_LATCH, mtr);
}

/** Initialize the doublewrite buffer data structure.
@param header   doublewrite page header in the TRX_SYS page */
inline void buf_dblwr_t::init(const byte *header)
{
  ut_ad(!active_slot->first_free);
  ut_ad(!active_slot->reserved);
  ut_ad(!batch_running);

  mysql_mutex_init(buf_dblwr_mutex_key, &mutex, nullptr);
  pthread_cond_init(&cond, nullptr);
  block1= page_id_t(0, mach_read_from_4(header + TRX_SYS_DOUBLEWRITE_BLOCK1));
  block2= page_id_t(0, mach_read_from_4(header + TRX_SYS_DOUBLEWRITE_BLOCK2));

  const uint32_t buf_size= 2 * block_size();
  for (int i= 0; i < 2; i++)
  {
    slots[i].write_buf= static_cast<byte*>
      (aligned_malloc(buf_size << srv_page_size_shift, srv_page_size));
    slots[i].buf_block_arr= static_cast<element*>
      (ut_zalloc_nokey(buf_size * sizeof(element)));
  }
  active_slot= &slots[0];
}

/** Create or restore the doublewrite buffer in the TRX_SYS page.
@return whether the operation succeeded */
bool buf_dblwr_t::create()
{
  if (is_initialised())
    return true;

  mtr_t mtr;
  const ulint size= block_size();

start_again:
  mtr.start();

  buf_block_t *trx_sys_block= buf_dblwr_trx_sys_get(&mtr);

  if (mach_read_from_4(TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC +
                       trx_sys_block->page.frame) ==
      TRX_SYS_DOUBLEWRITE_MAGIC_N)
  {
    /* The doublewrite buffer has already been created: just read in
    some numbers */
    init(TRX_SYS_DOUBLEWRITE + trx_sys_block->page.frame);
    mtr.commit();
    return true;
  }

  if (UT_LIST_GET_FIRST(fil_system.sys_space->chain)->size < 3 * size)
  {
too_small:
    ib::error() << "Cannot create doublewrite buffer: "
                   "the first file in innodb_data_file_path must be at least "
                << (3 * (size >> (20U - srv_page_size_shift))) << "M.";
    mtr.commit();
    return false;
  }
  else
  {
    buf_block_t *b= fseg_create(fil_system.sys_space,
                                TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_FSEG,
                                &mtr, false, trx_sys_block);
    if (!b)
      goto too_small;
    ib::info() << "Doublewrite buffer not found: creating new";

    /* FIXME: After this point, the doublewrite buffer creation
    is not atomic. The doublewrite buffer should not exist in
    the InnoDB system tablespace file in the first place.
    It could be located in separate optional file(s) in a
    user-specified location. */
  }

  byte *fseg_header= TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_FSEG +
    trx_sys_block->page.frame;
  for (uint32_t prev_page_no= 0, i= 0, extent_size= FSP_EXTENT_SIZE;
       i < 2 * size + extent_size / 2; i++)
  {
    buf_block_t *new_block= fseg_alloc_free_page(fseg_header, prev_page_no + 1,
                                                 FSP_UP, &mtr);
    if (!new_block)
    {
      ib::error() << "Cannot create doublewrite buffer: "
                     " you must increase your tablespace size."
                     " Cannot continue operation.";
      /* This may essentially corrupt the doublewrite
      buffer. However, usually the doublewrite buffer
      is created at database initialization, and it
      should not matter (just remove all newly created
      InnoDB files and restart). */
      mtr.commit();
      return false;
    }

    /* We read the allocated pages to the buffer pool; when they are
    written to disk in a flush, the space id and page number fields
    are also written to the pages. When we at database startup read
    pages from the doublewrite buffer, we know that if the space id
    and page number in them are the same as the page position in the
    tablespace, then the page has not been written to in
    doublewrite. */

    ut_ad(new_block->page.lock.not_recursive());
    const page_id_t id= new_block->page.id();
    /* We only do this in the debug build, to ensure that the check in
    buf_flush_init_for_writing() will see a valid page type. The
    flushes of new_block are actually unnecessary here.  */
    ut_d(mtr.write<2>(*new_block, FIL_PAGE_TYPE + new_block->page.frame,
                      FIL_PAGE_TYPE_SYS));

    if (i == size / 2)
    {
      ut_a(id.page_no() == size);
      mtr.write<4>(*trx_sys_block,
                   TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_BLOCK1 +
                   trx_sys_block->page.frame, id.page_no());
      mtr.write<4>(*trx_sys_block,
                   TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_REPEAT +
                   TRX_SYS_DOUBLEWRITE_BLOCK1 + trx_sys_block->page.frame,
                   id.page_no());
    }
    else if (i == size / 2 + size)
    {
      ut_a(id.page_no() == 2 * size);
      mtr.write<4>(*trx_sys_block,
                   TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_BLOCK2 +
                   trx_sys_block->page.frame, id.page_no());
      mtr.write<4>(*trx_sys_block,
                   TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_REPEAT +
                   TRX_SYS_DOUBLEWRITE_BLOCK2 + trx_sys_block->page.frame,
                   id.page_no());
    }
    else if (i > size / 2)
      ut_a(id.page_no() == prev_page_no + 1);

    if (((i + 1) & 15) == 0) {
      /* rw_locks can only be recursively x-locked 2048 times. (on 32
      bit platforms, (lint) 0 - (X_LOCK_DECR * 2049) is no longer a
      negative number, and thus lock_word becomes like a shared lock).
      For 4k page size this loop will lock the fseg header too many
      times. Since this code is not done while any other threads are
      active, restart the MTR occasionally. */
      mtr.commit();
      mtr.start();
      trx_sys_block= buf_dblwr_trx_sys_get(&mtr);
      fseg_header= TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_FSEG +
        trx_sys_block->page.frame;
    }

    prev_page_no= id.page_no();
  }

  mtr.write<4>(*trx_sys_block,
               TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC +
               trx_sys_block->page.frame, TRX_SYS_DOUBLEWRITE_MAGIC_N);
  mtr.write<4>(*trx_sys_block,
               TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC +
               TRX_SYS_DOUBLEWRITE_REPEAT + trx_sys_block->page.frame,
               TRX_SYS_DOUBLEWRITE_MAGIC_N);

  mtr.write<4>(*trx_sys_block,
               TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED +
               trx_sys_block->page.frame,
               TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N);
  mtr.commit();

  buf_flush_wait_flushed(mtr.commit_lsn());

  /* Remove doublewrite pages from LRU */
  buf_pool_invalidate();
  goto start_again;
}

/** Initialize the doublewrite buffer memory structure on recovery.
If we are upgrading from a version before MySQL 4.1, then this
function performs the necessary update operations to support
innodb_file_per_table. If we are in a crash recovery, this function
loads the pages from double write buffer into memory.
@param file File handle
@param path Path name of file
@return DB_SUCCESS or error code */
dberr_t buf_dblwr_t::init_or_load_pages(pfs_os_file_t file, const char *path)
{
  ut_ad(this == &buf_dblwr);
  const uint32_t size= block_size();

  /* We do the file i/o past the buffer pool */
  byte *read_buf= static_cast<byte*>(aligned_malloc(srv_page_size,
                                                    srv_page_size));
  /* Read the TRX_SYS header to check if we are using the doublewrite buffer */
  dberr_t err= os_file_read(IORequestRead, file, read_buf,
                            TRX_SYS_PAGE_NO << srv_page_size_shift,
                            srv_page_size);

  if (err != DB_SUCCESS)
  {
    ib::error() << "Failed to read the system tablespace header page";
func_exit:
    aligned_free(read_buf);
    return err;
  }

  /* TRX_SYS_PAGE_NO is not encrypted see fil_crypt_rotate_page() */
  if (mach_read_from_4(TRX_SYS_DOUBLEWRITE_MAGIC + TRX_SYS_DOUBLEWRITE +
                       read_buf) != TRX_SYS_DOUBLEWRITE_MAGIC_N)
  {
    /* There is no doublewrite buffer initialized in the TRX_SYS page.
    This should normally not be possible; the doublewrite buffer should
    be initialized when creating the database. */
    err= DB_SUCCESS;
    goto func_exit;
  }

  init(TRX_SYS_DOUBLEWRITE + read_buf);

  const bool upgrade_to_innodb_file_per_table=
    mach_read_from_4(TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED +
                     TRX_SYS_DOUBLEWRITE + read_buf) !=
    TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N;

  auto write_buf= active_slot->write_buf;
  /* Read the pages from the doublewrite buffer to memory */
  err= os_file_read(IORequestRead, file, write_buf,
                    block1.page_no() << srv_page_size_shift,
                    size << srv_page_size_shift);

  if (err != DB_SUCCESS)
  {
    ib::error() << "Failed to read the first double write buffer extent";
    goto func_exit;
  }

  err= os_file_read(IORequestRead, file,
                    write_buf + (size << srv_page_size_shift),
                    block2.page_no() << srv_page_size_shift,
                    size << srv_page_size_shift);
  if (err != DB_SUCCESS)
  {
    ib::error() << "Failed to read the second double write buffer extent";
    goto func_exit;
  }

  byte *page= write_buf;

  if (UNIV_UNLIKELY(upgrade_to_innodb_file_per_table))
  {
    ib::info() << "Resetting space id's in the doublewrite buffer";

    for (ulint i= 0; i < size * 2; i++, page += srv_page_size)
    {
      memset(page + FIL_PAGE_SPACE_ID, 0, 4);
      /* For pre-MySQL-4.1 innodb_checksum_algorithm=innodb, we do not need to
      calculate new checksums for the pages because the field
      .._SPACE_ID does not affect them. Write the page back to where
      we read it from. */
      const ulint source_page_no= i < size
        ? block1.page_no() + i
        : block2.page_no() + i - size;
      err= os_file_write(IORequestWrite, path, file, page,
                         source_page_no << srv_page_size_shift, srv_page_size);
      if (err != DB_SUCCESS)
      {
        ib::error() << "Failed to upgrade the double write buffer";
        goto func_exit;
      }
    }
    os_file_flush(file);
  }
  else
    for (ulint i= 0; i < size * 2; i++, page += srv_page_size)
      if (mach_read_from_8(my_assume_aligned<8>(page + FIL_PAGE_LSN)))
        /* Each valid page header must contain a nonzero FIL_PAGE_LSN field. */
        recv_sys.dblwr.add(page);

  err= DB_SUCCESS;
  goto func_exit;
}

/** Process and remove the double write buffer pages for all tablespaces. */
void buf_dblwr_t::recover()
{
  ut_ad(log_sys.last_checkpoint_lsn);
  if (!is_initialised())
    return;

  uint32_t page_no_dblwr= 0;
  byte *read_buf= static_cast<byte*>(aligned_malloc(3 * srv_page_size,
                                                    srv_page_size));
  byte *const buf= read_buf + srv_page_size;

  for (recv_dblwr_t::list::iterator i= recv_sys.dblwr.pages.begin();
       i != recv_sys.dblwr.pages.end(); ++i, ++page_no_dblwr)
  {
    byte *page= *i;
    const uint32_t page_no= page_get_page_no(page);
    if (!page_no) /* recovered via Datafile::restore_from_doublewrite() */
      continue;

    const lsn_t lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    if (log_sys.last_checkpoint_lsn > lsn)
      /* Pages written before the checkpoint are not useful for recovery. */
      continue;
    const uint32_t space_id= page_get_space_id(page);
    const page_id_t page_id(space_id, page_no);

    if (recv_sys.lsn < lsn)
    {
      ib::info() << "Ignoring a doublewrite copy of page " << page_id
                 << " with future log sequence number " << lsn;
      continue;
    }

    fil_space_t *space= fil_space_t::get(space_id);

    if (!space)
      /* The tablespace that this page once belonged to does not exist */
      continue;

    if (UNIV_UNLIKELY(page_no >= space->get_size()))
    {
      /* Do not report the warning for undo tablespaces, because they
      can be truncated in place. */
      if (!srv_is_undo_tablespace(space_id))
        ib::warn() << "A copy of page " << page_no
                   << " in the doublewrite buffer slot " << page_no_dblwr
                   << " is beyond the end of " << space->chain.start->name
                   << " (" << space->size << " pages)";
next_page:
      space->release();
      continue;
    }

    const ulint physical_size= space->physical_size();
    ut_ad(!buf_is_zeroes(span<const byte>(page, physical_size)));

    /* We want to ensure that for partial reads the unread portion of
    the page is NUL. */
    memset(read_buf, 0x0, physical_size);

    /* Read in the actual page from the file */
    fil_io_t fio= space->io(IORequest(IORequest::DBLWR_RECOVER),
                            os_offset_t{page_no} * physical_size,
                            physical_size, read_buf);

    if (UNIV_UNLIKELY(fio.err != DB_SUCCESS))
       ib::warn() << "Double write buffer recovery: " << page_id
                  << " ('" << space->chain.start->name
                  << "') read failed with error: " << fio.err;

    if (buf_is_zeroes(span<const byte>(read_buf, physical_size)))
    {
      /* We will check if the copy in the doublewrite buffer is
      valid. If not, we will ignore this page (there should be redo
      log records to initialize it). */
    }
    else if (recv_sys.dblwr.validate_page(page_id, read_buf, space, buf))
      goto next_page;
    else
      /* We intentionally skip this message for all-zero pages. */
      ib::info() << "Trying to recover page " << page_id
                 << " from the doublewrite buffer.";

    page= recv_sys.dblwr.find_page(page_id, space, buf);

    if (!page)
      goto next_page;

    /* Write the good page from the doublewrite buffer to the intended
    position. */
    space->reacquire();
    fio= space->io(IORequestWrite,
                   os_offset_t{page_id.page_no()} * physical_size,
                   physical_size, page);

    if (fio.err == DB_SUCCESS)
      ib::info() << "Recovered page " << page_id << " to '" << fio.node->name
                 << "' from the doublewrite buffer.";
    goto next_page;
  }

  recv_sys.dblwr.pages.clear();
  fil_flush_file_spaces();
  aligned_free(read_buf);
}

/** Free the doublewrite buffer. */
void buf_dblwr_t::close()
{
  if (!is_initialised())
    return;

  /* Free the double write data structures. */
  ut_ad(!active_slot->reserved);
  ut_ad(!active_slot->first_free);
  ut_ad(!batch_running);

  pthread_cond_destroy(&cond);
  for (int i= 0; i < 2; i++)
  {
    aligned_free(slots[i].write_buf);
    ut_free(slots[i].buf_block_arr);
  }
  mysql_mutex_destroy(&mutex);

  memset((void*) this, 0, sizeof *this);
  active_slot= &slots[0];
}

/** Update the doublewrite buffer on write completion. */
void buf_dblwr_t::write_completed()
{
  ut_ad(this == &buf_dblwr);
  ut_ad(srv_use_doublewrite_buf);
  ut_ad(is_initialised());
  ut_ad(!srv_read_only_mode);

  mysql_mutex_lock(&mutex);

  ut_ad(batch_running);
  slot *flush_slot= active_slot == &slots[0] ? &slots[1] : &slots[0];
  ut_ad(flush_slot->reserved);
  ut_ad(flush_slot->reserved <= flush_slot->first_free);

  if (!--flush_slot->reserved)
  {
    mysql_mutex_unlock(&mutex);
    /* This will finish the batch. Sync data files to the disk. */
    fil_flush_file_spaces();
    mysql_mutex_lock(&mutex);

    /* We can now reuse the doublewrite memory buffer: */
    flush_slot->first_free= 0;
    batch_running= false;
    pthread_cond_broadcast(&cond);
  }

  mysql_mutex_unlock(&mutex);
}

#ifdef UNIV_DEBUG
/** Check the LSN values on the page.
@param[in] page  page to check
@param[in] s     tablespace */
static void buf_dblwr_check_page_lsn(const page_t* page, const fil_space_t& s)
{
  /* Ignore page_compressed or encrypted pages */
  if (s.is_compressed() || buf_page_get_key_version(page, s.flags))
    return;
  const byte* lsn_start= FIL_PAGE_LSN + 4 + page;
  const byte* lsn_end= page + srv_page_size -
    (s.full_crc32()
     ? FIL_PAGE_FCRC32_END_LSN
     : FIL_PAGE_END_LSN_OLD_CHKSUM - 4);
  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
  ut_ad(!memcmp_aligned<4>(lsn_start, lsn_end, 4));
}

static void buf_dblwr_check_page_lsn(const buf_page_t &b, const byte *page)
{
  if (fil_space_t *space= fil_space_t::get(b.id().space()))
  {
    buf_dblwr_check_page_lsn(page, *space);
    space->release();
  }
}

/** Check the LSN values on the page with which this block is associated. */
static void buf_dblwr_check_block(const buf_page_t *bpage)
{
  ut_ad(bpage->in_file());
  const page_t *page= bpage->frame;
  ut_ad(page);

  switch (fil_page_get_type(page)) {
  case FIL_PAGE_INDEX:
  case FIL_PAGE_TYPE_INSTANT:
  case FIL_PAGE_RTREE:
    if (page_is_comp(page))
    {
      if (page_simple_validate_new(page))
        return;
    }
    else if (page_simple_validate_old(page))
      return;
    /* While it is possible that this is not an index page but just
    happens to have wrongly set FIL_PAGE_TYPE, such pages should never
    be modified to without also adjusting the page type during page
    allocation or buf_flush_init_for_writing() or
    fil_block_reset_type(). */
    buf_page_print(page);

    ib::fatal() << "Apparent corruption of an index page " << bpage->id()
                << " to be written to data file. We intentionally crash"
                " the server to prevent corrupt data from ending up in"
                " data files.";
  }
}
#endif /* UNIV_DEBUG */

bool buf_dblwr_t::flush_buffered_writes(const ulint size)
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(size == block_size());

  for (;;)
  {
    if (!active_slot->first_free)
      return false;
    if (!batch_running)
      break;
    my_cond_wait(&cond, &mutex.m_mutex);
  }

  ut_ad(active_slot->reserved == active_slot->first_free);
  ut_ad(!flushing_buffered_writes);

  /* Disallow anyone else to start another batch of flushing. */
  slot *flush_slot= active_slot;
  /* Switch the active slot */
  active_slot= active_slot == &slots[0] ? &slots[1] : &slots[0];
  ut_a(active_slot->first_free == 0);
  batch_running= true;
  const ulint old_first_free= flush_slot->first_free;
  auto write_buf= flush_slot->write_buf;
  const bool multi_batch= block1 + static_cast<uint32_t>(size) != block2 &&
    old_first_free > size;
  flushing_buffered_writes= 1 + multi_batch;
  pages_submitted+= old_first_free;
  /* Now safe to release the mutex. */
  mysql_mutex_unlock(&mutex);
#ifdef UNIV_DEBUG
  for (ulint len2= 0, i= 0; i < old_first_free; len2 += srv_page_size, i++)
  {
    buf_page_t *bpage= flush_slot->buf_block_arr[i].request.bpage;

    if (bpage->zip.data)
      /* No simple validate for ROW_FORMAT=COMPRESSED pages exists. */
      continue;

    /* Check that the actual page in the buffer pool is not corrupt
    and the LSN values are sane. */
    buf_dblwr_check_block(bpage);
    ut_d(buf_dblwr_check_page_lsn(*bpage, write_buf + len2));
  }
#endif /* UNIV_DEBUG */
  const IORequest request{nullptr, nullptr, fil_system.sys_space->chain.start,
                          IORequest::DBLWR_BATCH};
  ut_a(fil_system.sys_space->acquire());
  if (multi_batch)
  {
    fil_system.sys_space->reacquire();
    os_aio(request, write_buf,
           os_offset_t{block1.page_no()} << srv_page_size_shift,
           size << srv_page_size_shift);
    os_aio(request, write_buf + (size << srv_page_size_shift),
           os_offset_t{block2.page_no()} << srv_page_size_shift,
           (old_first_free - size) << srv_page_size_shift);
  }
  else
    os_aio(request, write_buf,
           os_offset_t{block1.page_no()} << srv_page_size_shift,
           old_first_free << srv_page_size_shift);
  return true;
}

static void *get_frame(const IORequest &request)
{
  if (request.slot)
    return request.slot->out_buf;
  const buf_page_t *bpage= request.bpage;
  return bpage->zip.data ? bpage->zip.data : bpage->frame;
}

void buf_dblwr_t::flush_buffered_writes_completed(const IORequest &request)
{
  ut_ad(this == &buf_dblwr);
  ut_ad(srv_use_doublewrite_buf);
  ut_ad(is_initialised());
  ut_ad(!srv_read_only_mode);
  ut_ad(!request.bpage);
  ut_ad(request.node == fil_system.sys_space->chain.start);
  ut_ad(request.type == IORequest::DBLWR_BATCH);
  mysql_mutex_lock(&mutex);
  ut_ad(batch_running);
  ut_ad(flushing_buffered_writes);
  ut_ad(flushing_buffered_writes <= 2);
  writes_completed++;
  if (UNIV_UNLIKELY(--flushing_buffered_writes))
  {
    mysql_mutex_unlock(&mutex);
    return;
  }

  slot *const flush_slot= active_slot == &slots[0] ? &slots[1] : &slots[0];
  ut_ad(flush_slot->reserved == flush_slot->first_free);
  /* increment the doublewrite flushed pages counter */
  pages_written+= flush_slot->first_free;
  mysql_mutex_unlock(&mutex);

  /* Now flush the doublewrite buffer data to disk */
  fil_system.sys_space->flush<false>();

  /* The writes have been flushed to disk now and in recovery we will
  find them in the doublewrite buffer blocks. Next, write the data pages. */
  for (ulint i= 0, first_free= flush_slot->first_free; i < first_free; i++)
  {
    auto e= flush_slot->buf_block_arr[i];
    buf_page_t* bpage= e.request.bpage;
    ut_ad(bpage->in_file());

    void *frame= get_frame(e.request);
    ut_ad(frame);

    auto e_size= e.size;

    if (UNIV_LIKELY_NULL(bpage->zip.data))
    {
      e_size= bpage->zip_size();
      ut_ad(e_size);
    }
    else
    {
      ut_ad(!bpage->zip_size());
      ut_d(buf_dblwr_check_page_lsn(*bpage, static_cast<const byte*>(frame)));
    }

    const lsn_t lsn= mach_read_from_8(my_assume_aligned<8>
                                      (FIL_PAGE_LSN +
                                       static_cast<const byte*>(frame)));
    ut_ad(lsn);
    ut_ad(lsn >= bpage->oldest_modification());
    log_write_up_to(lsn, true);
    e.request.node->space->io(e.request, bpage->physical_offset(), e_size,
                              frame, bpage);
  }
}

/** Flush possible buffered writes to persistent storage.
It is very important to call this function after a batch of writes has been
posted, and also when we may have to wait for a page latch!
Otherwise a deadlock of threads can occur. */
void buf_dblwr_t::flush_buffered_writes()
{
  if (!is_initialised() || !srv_use_doublewrite_buf)
  {
    fil_flush_file_spaces();
    return;
  }

  ut_ad(!srv_read_only_mode);
  const ulint size= block_size();

  mysql_mutex_lock(&mutex);
  if (!flush_buffered_writes(size))
    mysql_mutex_unlock(&mutex);
}

/** Schedule a page write. If the doublewrite memory buffer is full,
flush_buffered_writes() will be invoked to make space.
@param request    asynchronous write request
@param size       payload size in bytes */
void buf_dblwr_t::add_to_batch(const IORequest &request, size_t size)
{
  ut_ad(request.is_async());
  ut_ad(request.is_write());
  ut_ad(request.bpage);
  ut_ad(request.bpage->in_file());
  ut_ad(request.node);
  ut_ad(request.node->space->purpose == FIL_TYPE_TABLESPACE);
  ut_ad(request.node->space->id == request.bpage->id().space());
  ut_ad(request.node->space->referenced());
  ut_ad(!srv_read_only_mode);

  const ulint buf_size= 2 * block_size();

  mysql_mutex_lock(&mutex);

  for (;;)
  {
    ut_ad(active_slot->first_free <= buf_size);
    if (active_slot->first_free != buf_size)
      break;

    if (flush_buffered_writes(buf_size / 2))
      mysql_mutex_lock(&mutex);
  }

  byte *p= active_slot->write_buf + srv_page_size * active_slot->first_free;

  /* "frame" is at least 1024-byte aligned for ROW_FORMAT=COMPRESSED pages,
  and at least srv_page_size (4096-byte) for everything else. */
  memcpy_aligned<UNIV_ZIP_SIZE_MIN>(p, get_frame(request), size);
  /* fil_page_compress() for page_compressed guarantees 256-byte alignment */
  memset_aligned<256>(p + size, 0, srv_page_size - size);
  /* FIXME: Inform the compiler that "size" and "srv_page_size - size"
  are integer multiples of 256, so the above can translate into simple
  SIMD instructions. Currently, we make no such assumptions about the
  non-pointer parameters that are passed to the _aligned templates. */
  ut_ad(!request.bpage->zip_size() || request.bpage->zip_size() == size);
  ut_ad(active_slot->reserved == active_slot->first_free);
  ut_ad(active_slot->reserved < buf_size);
  new (active_slot->buf_block_arr + active_slot->first_free++)
    element{request, size};
  active_slot->reserved= active_slot->first_free;

  if (active_slot->first_free != buf_size ||
      !flush_buffered_writes(buf_size / 2))
    mysql_mutex_unlock(&mutex);
}
