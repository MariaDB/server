/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0log.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "page0types.h"
#include "log0crypt.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif
#include "srv0start.h"
#include "log.h"

void mtr_memo_slot_t::release() const
{
  ut_ad(object);

  switch (type) {
  case MTR_MEMO_S_LOCK:
    static_cast<index_lock*>(object)->s_unlock();
    break;
  case MTR_MEMO_X_LOCK:
  case MTR_MEMO_SX_LOCK:
    static_cast<index_lock*>(object)->
      u_or_x_unlock(type == MTR_MEMO_SX_LOCK);
    break;
  case MTR_MEMO_SPACE_X_LOCK:
    static_cast<fil_space_t*>(object)->set_committed_size();
    static_cast<fil_space_t*>(object)->x_unlock();
    break;
  default:
    buf_page_t *bpage= static_cast<buf_page_t*>(object);
    ut_d(const auto s=)
      bpage->unfix();
    ut_ad(s < buf_page_t::READ_FIX || s >= buf_page_t::WRITE_FIX);
    switch (type) {
    case MTR_MEMO_PAGE_S_FIX:
      bpage->lock.s_unlock();
      break;
    case MTR_MEMO_BUF_FIX:
      break;
    default:
      ut_ad(type == MTR_MEMO_PAGE_SX_FIX ||
            type == MTR_MEMO_PAGE_X_FIX ||
            type == MTR_MEMO_PAGE_SX_MODIFY ||
            type == MTR_MEMO_PAGE_X_MODIFY);
      bpage->lock.u_or_x_unlock(type & MTR_MEMO_PAGE_SX_FIX);
    }
  }
}

/** Prepare to insert a modified blcok into flush_list.
@param lsn start LSN of the mini-transaction
@return insert position for insert_into_flush_list() */
inline buf_page_t *buf_pool_t::prepare_insert_into_flush_list(lsn_t lsn)
  noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(recv_recovery_is_on() || log_sys.latch.is_locked());
#endif
  ut_ad(lsn >= log_sys.last_checkpoint_lsn);
  mysql_mutex_assert_owner(&flush_list_mutex);
  static_assert(log_t::FIRST_LSN >= 2, "compatibility");

rescan:
  buf_page_t *prev= UT_LIST_GET_FIRST(flush_list);
  if (prev)
  {
    lsn_t om= prev->oldest_modification();
    if (om == 1)
    {
      delete_from_flush_list(prev);
      goto rescan;
    }
    ut_ad(om > 2);
    if (om <= lsn)
      return nullptr;
    while (buf_page_t *next= UT_LIST_GET_NEXT(list, prev))
    {
      om= next->oldest_modification();
      if (om == 1)
      {
        delete_from_flush_list(next);
        continue;
      }
      ut_ad(om > 2);
      if (om <= lsn)
        break;
      prev= next;
    }
    flush_hp.adjust(prev);
  }
  return prev;
}

/** Insert a modified block into the flush list.
@param prev     insert position (from prepare_insert_into_flush_list())
@param block    modified block
@param lsn      start LSN of the mini-transaction that modified the block */
inline void buf_pool_t::insert_into_flush_list(buf_page_t *prev,
                                               buf_block_t *block, lsn_t lsn)
  noexcept
{
  ut_ad(!fsp_is_system_temporary(block->page.id().space()));
  mysql_mutex_assert_owner(&flush_list_mutex);

  MEM_CHECK_DEFINED(block->page.zip.data
                    ? block->page.zip.data : block->page.frame,
                    block->physical_size());

  if (const lsn_t old= block->page.oldest_modification())
  {
    if (old > 1)
      return;
    flush_hp.adjust(&block->page);
    UT_LIST_REMOVE(flush_list, &block->page);
  }
  else
    flush_list_bytes+= block->physical_size();

  ut_ad(flush_list_bytes <= curr_pool_size);

  if (prev)
    UT_LIST_INSERT_AFTER(flush_list, prev, &block->page);
  else
    UT_LIST_ADD_FIRST(flush_list, &block->page);

  block->page.set_oldest_modification(lsn);
}

mtr_t::mtr_t()= default;
mtr_t::~mtr_t()= default;

/** Start a mini-transaction. */
void mtr_t::start()
{
  ut_ad(m_memo.empty());
  ut_ad(!m_freed_pages);
  ut_ad(!m_freed_space);
  MEM_UNDEFINED(this, sizeof *this);
  MEM_MAKE_DEFINED(&m_memo, sizeof m_memo);
  MEM_MAKE_DEFINED(&m_freed_space, sizeof m_freed_space);
  MEM_MAKE_DEFINED(&m_freed_pages, sizeof m_freed_pages);

  ut_d(m_start= true);
  ut_d(m_commit= false);
  ut_d(m_freeing_tree= false);

  m_last= nullptr;
  m_last_offset= 0;

  new(&m_log) mtr_buf_t();

  m_made_dirty= false;
  m_latch_ex= false;
  m_inside_ibuf= false;
  m_modifications= false;
  m_log_mode= MTR_LOG_ALL;
  ut_d(m_user_space_id= TRX_SYS_SPACE);
  m_user_space= nullptr;
  m_commit_lsn= 0;
  m_trim_pages= false;
}

/** Release the resources */
inline void mtr_t::release_resources()
{
  ut_ad(is_active());
  ut_ad(m_memo.empty());
  m_log.erase();
  ut_d(m_commit= true);
}

/** Handle any pages that were freed during the mini-transaction. */
void mtr_t::process_freed_pages()
{
  if (m_freed_pages)
  {
    ut_ad(!m_freed_pages->empty());
    ut_ad(m_freed_space);
    ut_ad(m_freed_space->is_owner());
    ut_ad(is_named_space(m_freed_space));

    /* Update the last freed lsn */
    m_freed_space->freed_range_mutex.lock();
    m_freed_space->update_last_freed_lsn(m_commit_lsn);
    if (!m_trim_pages)
      for (const auto &range : *m_freed_pages)
        m_freed_space->add_free_range(range);
    else
      m_freed_space->clear_freed_ranges();
    m_freed_space->freed_range_mutex.unlock();

    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
    /* mtr_t::start() will reset m_trim_pages */
  }
  else
    ut_ad(!m_freed_space);
}

ATTRIBUTE_COLD __attribute__((noinline))
/** Insert a modified block into buf_pool.flush_list on IMPORT TABLESPACE. */
static void insert_imported(buf_block_t *block)
{
  if (block->page.oldest_modification() <= 1)
  {
    log_sys.latch.rd_lock(SRW_LOCK_CALL);
    const lsn_t lsn= log_sys.last_checkpoint_lsn;
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    buf_pool.insert_into_flush_list
      (buf_pool.prepare_insert_into_flush_list(lsn), block, lsn);
    log_sys.latch.rd_unlock();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }
}

/** Release modified pages when no log was written. */
void mtr_t::release_unlogged()
{
  ut_ad(m_log_mode == MTR_LOG_NO_REDO);
  ut_ad(m_log.size() == 0);

  process_freed_pages();

  for (auto it= m_memo.rbegin(); it != m_memo.rend(); it++)
  {
    mtr_memo_slot_t &slot= *it;
    ut_ad(slot.object);
    switch (slot.type) {
    case MTR_MEMO_S_LOCK:
      static_cast<index_lock*>(slot.object)->s_unlock();
      break;
    case MTR_MEMO_SPACE_X_LOCK:
      static_cast<fil_space_t*>(slot.object)->set_committed_size();
      static_cast<fil_space_t*>(slot.object)->x_unlock();
      break;
    case MTR_MEMO_X_LOCK:
    case MTR_MEMO_SX_LOCK:
      static_cast<index_lock*>(slot.object)->
        u_or_x_unlock(slot.type == MTR_MEMO_SX_LOCK);
      break;
    default:
      buf_block_t *block= static_cast<buf_block_t*>(slot.object);
      ut_d(const auto s=) block->page.unfix();
      ut_ad(s >= buf_page_t::FREED);
      ut_ad(s < buf_page_t::READ_FIX);

      if (slot.type & MTR_MEMO_MODIFY)
      {
        ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
              slot.type == MTR_MEMO_PAGE_SX_MODIFY);
        ut_ad(block->page.id() < end_page_id);
        insert_imported(block);
      }

      switch (slot.type) {
      case MTR_MEMO_PAGE_S_FIX:
        block->page.lock.s_unlock();
        break;
      case MTR_MEMO_BUF_FIX:
        break;
      default:
        ut_ad(slot.type == MTR_MEMO_PAGE_SX_FIX ||
              slot.type == MTR_MEMO_PAGE_X_FIX ||
              slot.type == MTR_MEMO_PAGE_SX_MODIFY ||
              slot.type == MTR_MEMO_PAGE_X_MODIFY);
        block->page.lock.u_or_x_unlock(slot.type & MTR_MEMO_PAGE_SX_FIX);
      }
    }
  }

  m_memo.clear();
}

void mtr_t::release()
{
  for (auto it= m_memo.rbegin(); it != m_memo.rend(); it++)
    it->release();
  m_memo.clear();
}

/** Commit a mini-transaction. */
void mtr_t::commit()
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());

  /* This is a dirty read, for debugging. */
  ut_ad(!m_modifications || !recv_no_log_write);
  ut_ad(!m_modifications || m_log_mode != MTR_LOG_NONE);
  ut_ad(!m_latch_ex);

  if (m_modifications && (m_log_mode == MTR_LOG_NO_REDO || !m_log.empty()))
  {
    if (UNIV_UNLIKELY(!is_logged()))
    {
      release_unlogged();
      goto func_exit;
    }

    ut_ad(!srv_read_only_mode);
    std::pair<lsn_t,page_flush_ahead> lsns{do_write()};
    process_freed_pages();

    if (m_made_dirty)
    {
      size_t modified= 0;
      auto it= m_memo.rbegin();

      mysql_mutex_lock(&buf_pool.flush_list_mutex);

      buf_page_t *const prev=
        buf_pool.prepare_insert_into_flush_list(lsns.first);

      while (it != m_memo.rend())
      {
        const mtr_memo_slot_t &slot= *it++;
        if (slot.type & MTR_MEMO_MODIFY)
        {
          ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
                slot.type == MTR_MEMO_PAGE_SX_MODIFY);
          modified++;
          buf_block_t *b= static_cast<buf_block_t*>(slot.object);
          ut_ad(b->page.id() < end_page_id);
          ut_d(const auto s= b->page.state());
          ut_ad(s > buf_page_t::FREED);
          ut_ad(s < buf_page_t::READ_FIX);
          ut_ad(mach_read_from_8(b->page.frame + FIL_PAGE_LSN) <=
                m_commit_lsn);
          mach_write_to_8(b->page.frame + FIL_PAGE_LSN, m_commit_lsn);
          if (UNIV_LIKELY_NULL(b->page.zip.data))
            memcpy_aligned<8>(FIL_PAGE_LSN + b->page.zip.data,
                              FIL_PAGE_LSN + b->page.frame, 8);
          buf_pool.insert_into_flush_list(prev, b, lsns.first);
        }
      }

      ut_ad(modified);
      buf_pool.flush_list_requests+= modified;
      buf_pool.page_cleaner_wakeup();
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);

      if (m_latch_ex)
      {
        log_sys.latch.wr_unlock();
        m_latch_ex= false;
      }
      else
        log_sys.latch.rd_unlock();

      release();
    }
    else
    {
      if (m_latch_ex)
      {
        log_sys.latch.wr_unlock();
        m_latch_ex= false;
      }
      else
        log_sys.latch.rd_unlock();

      size_t modified= 0;

      for (auto it= m_memo.rbegin(); it != m_memo.rend(); )
      {
        const mtr_memo_slot_t &slot= *it++;
        ut_ad(slot.object);
        switch (slot.type) {
        case MTR_MEMO_S_LOCK:
          static_cast<index_lock*>(slot.object)->s_unlock();
          break;
        case MTR_MEMO_SPACE_X_LOCK:
          static_cast<fil_space_t*>(slot.object)->set_committed_size();
          static_cast<fil_space_t*>(slot.object)->x_unlock();
          break;
        case MTR_MEMO_X_LOCK:
        case MTR_MEMO_SX_LOCK:
          static_cast<index_lock*>(slot.object)->
            u_or_x_unlock(slot.type == MTR_MEMO_SX_LOCK);
          break;
        default:
          buf_page_t *bpage= static_cast<buf_page_t*>(slot.object);
          const auto s= bpage->unfix();
          if (slot.type & MTR_MEMO_MODIFY)
          {
            ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
                  slot.type == MTR_MEMO_PAGE_SX_MODIFY);
            ut_ad(bpage->oldest_modification() > 1);
            ut_ad(bpage->oldest_modification() < m_commit_lsn);
            ut_ad(bpage->id() < end_page_id);
            ut_ad(s >= buf_page_t::FREED);
            ut_ad(s < buf_page_t::READ_FIX);
            ut_ad(mach_read_from_8(bpage->frame + FIL_PAGE_LSN) <=
                  m_commit_lsn);
            if (s >= buf_page_t::UNFIXED)
            {
              mach_write_to_8(bpage->frame + FIL_PAGE_LSN, m_commit_lsn);
              if (UNIV_LIKELY_NULL(bpage->zip.data))
                memcpy_aligned<8>(FIL_PAGE_LSN + bpage->zip.data,
                                  FIL_PAGE_LSN + bpage->frame, 8);
            }
            modified++;
          }
          switch (auto latch= slot.type & ~MTR_MEMO_MODIFY) {
          case MTR_MEMO_PAGE_S_FIX:
            bpage->lock.s_unlock();
            continue;
          case MTR_MEMO_PAGE_SX_FIX:
          case MTR_MEMO_PAGE_X_FIX:
            bpage->lock.u_or_x_unlock(latch == MTR_MEMO_PAGE_SX_FIX);
            continue;
          default:
            ut_ad(latch == MTR_MEMO_BUF_FIX);
          }
        }
      }

      buf_pool.add_flush_list_requests(modified);
      m_memo.clear();
    }

    if (UNIV_UNLIKELY(lsns.second != PAGE_FLUSH_NO))
      buf_flush_ahead(m_commit_lsn, lsns.second == PAGE_FLUSH_SYNC);
  }
  else
  {
    if (m_freed_pages)
    {
      ut_ad(!m_freed_pages->empty());
      ut_ad(m_freed_space == fil_system.temp_space);
      ut_ad(!m_trim_pages);
      for (const auto &range : *m_freed_pages)
        m_freed_space->add_free_range(range);
      delete m_freed_pages;
      m_freed_pages= nullptr;
      m_freed_space= nullptr;
    }
    release();
  }

func_exit:
  release_resources();
}

void mtr_t::rollback_to_savepoint(ulint begin, ulint end)
{
  ut_ad(end <= m_memo.size());
  ut_ad(begin <= end);
  ulint s= end;

  while (s-- > begin)
  {
    const mtr_memo_slot_t &slot= m_memo[s];
    ut_ad(slot.object);
    /* This is intended for releasing latches on indexes or unmodified
    buffer pool pages. */
    ut_ad(slot.type <= MTR_MEMO_SX_LOCK);
    ut_ad(!(slot.type & MTR_MEMO_MODIFY));
    slot.release();
  }

  m_memo.erase(m_memo.begin() + begin, m_memo.begin() + end);
}

/** Commit a mini-transaction that is shrinking a tablespace.
@param space   tablespace that is being shrunk */
void mtr_t::commit_shrink(fil_space_t &space)
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(!high_level_read_only);
  ut_ad(m_modifications);
  ut_ad(m_made_dirty);
  ut_ad(!m_memo.empty());
  ut_ad(!recv_recovery_is_on());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(!m_freed_pages);
  ut_ad(UT_LIST_GET_LEN(space.chain) == 1);

  log_write_and_flush_prepare();
  m_latch_ex= true;
  log_sys.latch.wr_lock(SRW_LOCK_CALL);

  const lsn_t start_lsn= do_write().first;
  ut_d(m_log.erase());

  /* Durably write the reduced FSP_SIZE before truncating the data file. */
  log_write_and_flush();
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif

  os_file_truncate(space.chain.start->name, space.chain.start->handle,
                   os_offset_t{space.size} << srv_page_size_shift, true);

  space.clear_freed_ranges();

  const page_id_t high{space.id, space.size};
  size_t modified= 0;
  auto it= m_memo.rbegin();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  buf_page_t *const prev= buf_pool.prepare_insert_into_flush_list(start_lsn);

  while (it != m_memo.rend())
  {
    mtr_memo_slot_t &slot= *it++;

    ut_ad(slot.object);
    if (slot.type == MTR_MEMO_SPACE_X_LOCK)
      ut_ad(high.space() == static_cast<fil_space_t*>(slot.object)->id);
    else
    {
      ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
            slot.type == MTR_MEMO_PAGE_SX_MODIFY ||
            slot.type == MTR_MEMO_PAGE_X_FIX ||
            slot.type == MTR_MEMO_PAGE_SX_FIX);
      buf_block_t *b= static_cast<buf_block_t*>(slot.object);
      const page_id_t id{b->page.id()};
      const auto s= b->page.state();
      ut_ad(s > buf_page_t::FREED);
      ut_ad(s < buf_page_t::READ_FIX);
      ut_ad(b->page.frame);
      ut_ad(mach_read_from_8(b->page.frame + FIL_PAGE_LSN) <= m_commit_lsn);
      ut_ad(!b->page.zip.data); // we no not shrink ROW_FORMAT=COMPRESSED

      if (id < high)
      {
        ut_ad(id.space() == high.space() ||
              (id == page_id_t{0, TRX_SYS_PAGE_NO} &&
               srv_is_undo_tablespace(high.space())));
        if (slot.type & MTR_MEMO_MODIFY)
        {
          modified++;
          mach_write_to_8(b->page.frame + FIL_PAGE_LSN, m_commit_lsn);
          buf_pool.insert_into_flush_list(prev, b, start_lsn);
        }
      }
      else
      {
        ut_ad(id.space() == high.space());
        if (s >= buf_page_t::UNFIXED)
          b->page.set_freed(s);
        if (b->page.oldest_modification() > 1)
          b->page.reset_oldest_modification();
        slot.type= mtr_memo_type_t(slot.type & ~MTR_MEMO_MODIFY);
      }
    }
  }

  ut_ad(modified);
  buf_pool.flush_list_requests+= modified;
  buf_pool.page_cleaner_wakeup();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  log_sys.latch.wr_unlock();
  m_latch_ex= false;

  mysql_mutex_lock(&fil_system.mutex);
  ut_ad(space.is_being_truncated);
  ut_ad(space.is_stopping());
  space.clear_stopping();
  space.is_being_truncated= false;
  mysql_mutex_unlock(&fil_system.mutex);

  release();
  release_resources();
}

/** Commit a mini-transaction that is deleting or renaming a file.
@param space   tablespace that is being renamed or deleted
@param name    new file name (nullptr=the file will be deleted)
@return whether the operation succeeded */
bool mtr_t::commit_file(fil_space_t &space, const char *name)
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(!high_level_read_only);
  ut_ad(m_modifications);
  ut_ad(!m_made_dirty);
  ut_ad(!recv_recovery_is_on());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(UT_LIST_GET_LEN(space.chain) == 1);
  ut_ad(!m_latch_ex);

  m_latch_ex= true;

  log_write_and_flush_prepare();

  log_sys.latch.wr_lock(SRW_LOCK_CALL);

  size_t size= m_log.size() + 5;

  if (log_sys.is_encrypted())
  {
    /* We will not encrypt any FILE_ records, but we will reserve
    a nonce at the end. */
    size+= 8;
    m_commit_lsn= log_sys.get_lsn();
  }
  else
    m_commit_lsn= 0;

  m_crc= 0;
  m_log.for_each_block([this](const mtr_buf_t::block_t *b)
  { m_crc= my_crc32c(m_crc, b->begin(), b->used()); return true; });
  finish_write(size);

  if (!name && space.max_lsn)
  {
    ut_d(space.max_lsn= 0);
    fil_system.named_spaces.remove(space);
  }

  /* Block log_checkpoint(). */
  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  /* Durably write the log for the file system operation. */
  log_write_and_flush();

  log_sys.latch.wr_unlock();
  m_latch_ex= false;

  char *old_name= space.chain.start->name;
  bool success;

  if (name)
  {
    char *new_name= mem_strdup(name);
    mysql_mutex_lock(&fil_system.mutex);
    success= os_file_rename(innodb_data_file_key, old_name, name);
    if (success)
      space.chain.start->name= new_name;
    else
      old_name= new_name;
    mysql_mutex_unlock(&fil_system.mutex);
    ut_free(old_name);
  }
  else
  {
    /* Remove any additional files. */
    if (char *cfg_name= fil_make_filepath(old_name,
					  fil_space_t::name_type{}, CFG,
                                          false))
    {
      os_file_delete_if_exists(innodb_data_file_key, cfg_name, nullptr);
      ut_free(cfg_name);
    }

    if (FSP_FLAGS_HAS_DATA_DIR(space.flags))
      RemoteDatafile::delete_link_file(space.name());

    /* Remove the directory entry. The file will actually be deleted
    when our caller closes the handle. */
    os_file_delete(innodb_data_file_key, old_name);

    mysql_mutex_lock(&fil_system.mutex);
    /* Sanity checks after reacquiring fil_system.mutex */
    ut_ad(&space == fil_space_get_by_id(space.id));
    ut_ad(!space.referenced());
    ut_ad(space.is_stopping());

    fil_system.detach(&space, true);
    mysql_mutex_unlock(&fil_system.mutex);

    success= true;
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  release_resources();

  return success;
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
FILE_MODIFY records and an optional FILE_CHECKPOINT marker.
The caller must hold exclusive log_sys.latch.
This is to be used at log_checkpoint().
@param checkpoint_lsn   the log sequence number of a checkpoint, or 0
@return current LSN */
lsn_t mtr_t::commit_files(lsn_t checkpoint_lsn)
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(!m_made_dirty);
  ut_ad(m_memo.empty());
  ut_ad(!srv_read_only_mode);
  ut_ad(!m_freed_space);
  ut_ad(!m_freed_pages);
  ut_ad(!m_user_space);
  ut_ad(!m_latch_ex);

  m_latch_ex= true;

  if (checkpoint_lsn)
  {
    byte *ptr= m_log.push<byte*>(3 + 8);
    *ptr= FILE_CHECKPOINT | (2 + 8);
    ::memset(ptr + 1, 0, 2);
    mach_write_to_8(ptr + 3, checkpoint_lsn);
  }

  size_t size= m_log.size() + 5;

  if (log_sys.is_encrypted())
  {
    /* We will not encrypt any FILE_ records, but we will reserve
    a nonce at the end. */
    size+= 8;
    m_commit_lsn= log_sys.get_lsn();
  }
  else
    m_commit_lsn= 0;

  m_crc= 0;
  m_log.for_each_block([this](const mtr_buf_t::block_t *b)
  { m_crc= my_crc32c(m_crc, b->begin(), b->used()); return true; });
  finish_write(size);
  release_resources();

  if (checkpoint_lsn)
    DBUG_PRINT("ib_log",
               ("FILE_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
                checkpoint_lsn, m_commit_lsn));

  return m_commit_lsn;
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(uint32_t space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);
  return !is_logged() || m_user_space_id == space ||
    is_predefined_tablespace(space);
}
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool mtr_t::is_named_space(const fil_space_t* space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

  return !is_logged() || m_user_space == space ||
    is_predefined_tablespace(space->id);
}
#endif /* UNIV_DEBUG */

/** Acquire a tablespace X-latch.
@param[in]	space_id	tablespace ID
@return the tablespace object (never NULL) */
fil_space_t *mtr_t::x_lock_space(uint32_t space_id)
{
	fil_space_t*	space;

	ut_ad(is_active());

	if (space_id == TRX_SYS_SPACE) {
		space = fil_system.sys_space;
	} else if ((space = m_user_space) && space_id == space->id) {
	} else {
		space = fil_space_get(space_id);
		ut_ad(m_log_mode != MTR_LOG_NO_REDO
		      || space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT);
	}

	ut_ad(space);
	ut_ad(space->id == space_id);
	x_lock_space(space);
	return(space);
}

/** Acquire an exclusive tablespace latch.
@param space  tablespace */
void mtr_t::x_lock_space(fil_space_t *space)
{
  ut_ad(space->purpose == FIL_TYPE_TEMPORARY ||
        space->purpose == FIL_TYPE_IMPORT ||
        space->purpose == FIL_TYPE_TABLESPACE);
  if (!memo_contains(*space))
  {
    memo_push(space, MTR_MEMO_SPACE_X_LOCK);
    space->x_lock();
  }
}

void mtr_t::release(const void *object)
{
  ut_ad(is_active());

  auto it=
    std::find_if(m_memo.begin(), m_memo.end(),
                 [object](const mtr_memo_slot_t& slot)
                 { return slot.object == object; });
  ut_ad(it != m_memo.end());
  ut_ad(!(it->type & MTR_MEMO_MODIFY));
  it->release();
  m_memo.erase(it, it + 1);
  ut_ad(std::find_if(m_memo.begin(), m_memo.end(),
                     [object](const mtr_memo_slot_t& slot)
                     { return slot.object == &object; }) == m_memo.end());
}

static time_t log_close_warn_time;

/** Display a warning that the log tail is overwriting the head,
making the server crash-unsafe. */
ATTRIBUTE_COLD static void log_overwrite_warning(lsn_t lsn)
{
  if (log_sys.overwrite_warned)
    return;

  time_t t= time(nullptr);
  if (difftime(t, log_close_warn_time) < 15)
    return;

  if (!log_sys.overwrite_warned)
    log_sys.overwrite_warned= lsn;
  log_close_warn_time= t;

  sql_print_error("InnoDB: Crash recovery is broken due to"
                  " insufficient innodb_log_file_size;"
                  " last checkpoint LSN=" LSN_PF ", current LSN=" LSN_PF
                  "%s.",
                  lsn_t{log_sys.last_checkpoint_lsn}, lsn,
                  srv_shutdown_state != SRV_SHUTDOWN_INITIATED
                  ? ". Shutdown is in progress" : "");
}

/** Wait in append_prepare() for buffer to become available
@param ex   whether log_sys.latch is exclusively locked */
ATTRIBUTE_COLD void log_t::append_prepare_wait(bool ex) noexcept
{
  log_sys.waits++;
  log_sys.unlock_lsn();

  if (ex)
    log_sys.latch.wr_unlock();
  else
    log_sys.latch.rd_unlock();

  DEBUG_SYNC_C("log_buf_size_exceeded");
  log_buffer_flush_to_disk(log_sys.is_pmem());

  if (ex)
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
  else
    log_sys.latch.rd_lock(SRW_LOCK_CALL);

  log_sys.lock_lsn();
}

/** Reserve space in the log buffer for appending data.
@tparam pmem  log_sys.is_pmem()
@param size   total length of the data to append(), in bytes
@param ex     whether log_sys.latch is exclusively locked
@return the start LSN and the buffer position for append() */
template<bool pmem>
inline
std::pair<lsn_t,byte*> log_t::append_prepare(size_t size, bool ex) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_locked());
# ifndef _WIN32 // there is no accurate is_write_locked() on SRWLOCK
  ut_ad(ex == latch.is_write_locked());
# endif
#endif
  ut_ad(pmem == is_pmem());
  const lsn_t checkpoint_margin{last_checkpoint_lsn + log_capacity - size};
  const size_t avail{(pmem ? size_t(capacity()) : buf_size) - size};
  lock_lsn();
  write_to_buf++;

  for (ut_d(int count= 50);
       UNIV_UNLIKELY((pmem
                      ? size_t(get_lsn() -
                               get_flushed_lsn(std::memory_order_relaxed))
                      : size_t{buf_free}) > avail); )
  {
    append_prepare_wait(ex);
    ut_ad(count--);
  }

  const lsn_t l{lsn.load(std::memory_order_relaxed)};
  lsn.store(l + size, std::memory_order_relaxed);
  const size_t b{buf_free};
  size_t new_buf_free{b};
  new_buf_free+= size;
  if (pmem && new_buf_free >= file_size)
    new_buf_free-= size_t(capacity());
  buf_free= new_buf_free;
  unlock_lsn();

  if (UNIV_UNLIKELY(l > checkpoint_margin) ||
      (!pmem && b >= max_buf_free))
    set_check_flush_or_checkpoint();

  return {l, &buf[b]};
}

/** Finish appending data to the log.
@param lsn  the end LSN of the log record
@return whether buf_flush_ahead() will have to be invoked */
static mtr_t::page_flush_ahead log_close(lsn_t lsn) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_locked());
#endif

  const lsn_t checkpoint_age= lsn - log_sys.last_checkpoint_lsn;

  if (UNIV_UNLIKELY(checkpoint_age >= log_sys.log_capacity) &&
      /* silence message on create_log_file() after the log had been deleted */
      checkpoint_age != lsn)
    log_overwrite_warning(lsn);
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_modified_age_async))
    return mtr_t::PAGE_FLUSH_NO;
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_checkpoint_age))
    return mtr_t::PAGE_FLUSH_ASYNC;

  log_sys.set_check_flush_or_checkpoint();
  return mtr_t::PAGE_FLUSH_SYNC;
}

inline void mtr_t::page_checksum(const buf_page_t &bpage)
{
  const byte *page= bpage.frame;
  size_t size= srv_page_size;

  if (UNIV_LIKELY_NULL(bpage.zip.data))
  {
    size= (UNIV_ZIP_SIZE_MIN >> 1) << bpage.zip.ssize;
    switch (fil_page_get_type(bpage.zip.data)) {
    case FIL_PAGE_TYPE_ALLOCATED:
    case FIL_PAGE_INODE:
    case FIL_PAGE_IBUF_BITMAP:
    case FIL_PAGE_TYPE_FSP_HDR:
    case FIL_PAGE_TYPE_XDES:
      /* These are essentially uncompressed pages. */
      break;
    default:
      page= bpage.zip.data;
    }
  }

  /* We have to exclude from the checksum the normal
  page checksum that is written by buf_flush_init_for_writing()
  and FIL_PAGE_LSN which would be updated once we have actually
  allocated the LSN.

  Unfortunately, we cannot access fil_space_t easily here. In order to
  be compatible with encrypted tablespaces in the pre-full_crc32
  format we will unconditionally exclude the 8 bytes at
  FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
  a.k.a. FIL_RTREE_SPLIT_SEQ_NUM. */
  const uint32_t checksum=
    my_crc32c(my_crc32c(my_crc32c(0, page + FIL_PAGE_OFFSET,
                                  FIL_PAGE_LSN - FIL_PAGE_OFFSET),
                        page + FIL_PAGE_TYPE, 2),
              page + FIL_PAGE_SPACE_ID, size - (FIL_PAGE_SPACE_ID + 8));

  byte *l= log_write<OPTION>(bpage.id(), nullptr, 5, true, 0);
  *l++= OPT_PAGE_CHECKSUM;
  mach_write_to_4(l, checksum);
  m_log.close(l + 4);
}

std::pair<lsn_t,mtr_t::page_flush_ahead> mtr_t::do_write()
{
  ut_ad(!recv_no_log_write);
  ut_ad(is_logged());
  ut_ad(m_log.size());
#ifndef SUX_LOCK_GENERIC
  ut_ad(!m_latch_ex || log_sys.latch.is_write_locked());
#endif

#ifndef DBUG_OFF
  do
  {
    if (m_log_mode != MTR_LOG_ALL)
      continue;
    DBUG_EXECUTE_IF("skip_page_checksum", continue;);

    for (const mtr_memo_slot_t& slot : m_memo)
      if (slot.type & MTR_MEMO_MODIFY)
      {
        const buf_page_t &b= *static_cast<const buf_page_t*>(slot.object);
        if (!b.is_freed())
          page_checksum(b);
      }
  }
  while (0);
#endif

  size_t len= m_log.size() + 5;
  ut_ad(len > 5);

  if (log_sys.is_encrypted())
  {
    len+= 8;
    encrypt();
  }
  else
  {
    m_crc= 0;
    m_commit_lsn= 0;
    m_log.for_each_block([this](const mtr_buf_t::block_t *b)
    { m_crc= my_crc32c(m_crc, b->begin(), b->used()); return true; });
  }

  if (!m_latch_ex)
    log_sys.latch.rd_lock(SRW_LOCK_CALL);

  if (UNIV_UNLIKELY(m_user_space && !m_user_space->max_lsn &&
                    !is_predefined_tablespace(m_user_space->id)))
  {
    if (!m_latch_ex)
    {
      m_latch_ex= true;
      log_sys.latch.rd_unlock();
      log_sys.latch.wr_lock(SRW_LOCK_CALL);
      if (UNIV_UNLIKELY(m_user_space->max_lsn != 0))
        goto func_exit;
    }
    name_write();
  }
func_exit:
  return finish_write(len);
}

/** Write the mini-transaction log to the redo log buffer.
@param len   number of bytes to write
@return {start_lsn,flush_ahead} */
std::pair<lsn_t,mtr_t::page_flush_ahead>
mtr_t::finish_write(size_t len)
{
  ut_ad(!recv_no_log_write);
  ut_ad(is_logged());
#ifndef SUX_LOCK_GENERIC
# ifndef _WIN32 // there is no accurate is_write_locked() on SRWLOCK
  ut_ad(m_latch_ex == log_sys.latch.is_write_locked());
# endif
#endif

  const size_t size{m_commit_lsn ? 5U + 8U : 5U};
  std::pair<lsn_t, byte*> start;

  if (!log_sys.is_pmem())
  {
    start= log_sys.append_prepare<false>(len, m_latch_ex);
    m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
    { log_sys.append(start.second, b->begin(), b->used()); return true; });

#ifdef HAVE_PMEM
  write_trailer:
#endif
    *start.second++= log_sys.get_sequence_bit(start.first + len - size);
    if (m_commit_lsn)
    {
      mach_write_to_8(start.second, m_commit_lsn);
      m_crc= my_crc32c(m_crc, start.second, 8);
      start.second+= 8;
    }
    mach_write_to_4(start.second, m_crc);
  }
#ifdef HAVE_PMEM
  else
  {
    start= log_sys.append_prepare<true>(len, m_latch_ex);
    if (UNIV_LIKELY(start.second + len <= &log_sys.buf[log_sys.file_size]))
    {
      m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
      { log_sys.append(start.second, b->begin(), b->used()); return true; });
      goto write_trailer;
    }
    m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
    {
      size_t size{b->used()};
      const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
      const byte *src= b->begin();
      if (size > size_left)
      {
        ::memcpy(start.second, src, size_left);
        start.second= &log_sys.buf[log_sys.START_OFFSET];
        src+= size_left;
        size-= size_left;
      }
      ::memcpy(start.second, src, size);
      start.second+= size;
      return true;
    });
    const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
    if (size_left > size)
      goto write_trailer;

    byte tail[5 + 8];
    tail[0]= log_sys.get_sequence_bit(start.first + len - size);

    if (m_commit_lsn)
    {
      mach_write_to_8(tail + 1, m_commit_lsn);
      m_crc= my_crc32c(m_crc, tail + 1, 8);
      mach_write_to_4(tail + 9, m_crc);
    }
    else
      mach_write_to_4(tail + 1, m_crc);

    ::memcpy(start.second, tail, size_left);
    ::memcpy(log_sys.buf + log_sys.START_OFFSET, tail + size_left,
             size - size_left);
  }
#endif

  m_commit_lsn= start.first + len;
  return {start.first, log_close(m_commit_lsn)};
}

bool mtr_t::have_x_latch(const buf_block_t &block) const
{
  ut_d(const mtr_memo_slot_t *found= nullptr);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object != &block)
      continue;

    ut_d(found= &slot);

    if (!(slot.type & MTR_MEMO_PAGE_X_FIX))
      continue;

    ut_ad(block.page.lock.have_x());
    return true;
  }

  ut_ad(!found);
  return false;
}

bool mtr_t::have_u_or_x_latch(const buf_block_t &block) const
{
  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &block &&
        slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX))
    {
      ut_ad(block.page.lock.have_u_or_x());
      return true;
    }
  }
  return false;
}

/** Check if we are holding exclusive tablespace latch
@param space  tablespace to search for
@return whether space.latch is being held */
bool mtr_t::memo_contains(const fil_space_t& space) const
{
  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &space && slot.type == MTR_MEMO_SPACE_X_LOCK)
    {
      ut_ad(space.is_owner());
      return true;
    }
  }

  return false;
}

void mtr_t::page_lock_upgrade(const buf_block_t &block)
{
  ut_ad(block.page.lock.have_x());

  for (mtr_memo_slot_t &slot : m_memo)
    if (slot.object == &block && slot.type & MTR_MEMO_PAGE_SX_FIX)
      slot.type= mtr_memo_type_t(slot.type ^
                                 (MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX));

#ifdef BTR_CUR_HASH_ADAPT
  ut_ad(!block.index || !block.index->freed());
#endif /* BTR_CUR_HASH_ADAPT */
}

/** Latch a buffer pool block.
@param block    block to be latched
@param rw_latch RW_S_LATCH, RW_SX_LATCH, RW_X_LATCH, RW_NO_LATCH */
void mtr_t::page_lock(buf_block_t *block, ulint rw_latch)
{
  mtr_memo_type_t fix_type;
  ut_d(const auto state= block->page.state());
  ut_ad(state > buf_page_t::FREED);
  ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);
  switch (rw_latch) {
  case RW_NO_LATCH:
    fix_type= MTR_MEMO_BUF_FIX;
    goto done;
  case RW_S_LATCH:
    fix_type= MTR_MEMO_PAGE_S_FIX;
    block->page.lock.s_lock();
    break;
  case RW_SX_LATCH:
    fix_type= MTR_MEMO_PAGE_SX_FIX;
    block->page.lock.u_lock();
    ut_ad(!block->page.is_io_fixed());
    break;
  default:
    ut_ad(rw_latch == RW_X_LATCH);
    fix_type= MTR_MEMO_PAGE_X_FIX;
    if (block->page.lock.x_lock_upgraded())
    {
      block->unfix();
      page_lock_upgrade(*block);
      return;
    }
    ut_ad(!block->page.is_io_fixed());
  }

#ifdef BTR_CUR_HASH_ADAPT
  btr_search_drop_page_hash_index(block, true);
#endif

done:
  ut_ad(state < buf_page_t::UNFIXED ||
        page_id_t(page_get_space_id(block->page.frame),
                  page_get_page_no(block->page.frame)) == block->page.id());
  memo_push(block, fix_type);
}

void mtr_t::upgrade_buffer_fix(ulint savepoint, rw_lock_type_t rw_latch)
{
  ut_ad(is_active());
  mtr_memo_slot_t &slot= m_memo[savepoint];
  ut_ad(slot.type == MTR_MEMO_BUF_FIX);
  buf_block_t *block= static_cast<buf_block_t*>(slot.object);
  ut_d(const auto state= block->page.state());
  ut_ad(state > buf_page_t::UNFIXED);
  ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);
  static_assert(int{MTR_MEMO_PAGE_S_FIX} == int{RW_S_LATCH}, "");
  static_assert(int{MTR_MEMO_PAGE_X_FIX} == int{RW_X_LATCH}, "");
  static_assert(int{MTR_MEMO_PAGE_SX_FIX} == int{RW_SX_LATCH}, "");
  slot.type= mtr_memo_type_t(rw_latch);

  switch (rw_latch) {
  default:
    ut_ad("invalid state" == 0);
    break;
  case RW_S_LATCH:
    block->page.lock.s_lock();
    break;
  case RW_SX_LATCH:
    block->page.lock.u_lock();
    ut_ad(!block->page.is_io_fixed());
    break;
  case RW_X_LATCH:
    block->page.lock.x_lock();
    ut_ad(!block->page.is_io_fixed());
  }

#ifdef BTR_CUR_HASH_ADAPT
  btr_search_drop_page_hash_index(block, true);
#endif
  ut_ad(page_id_t(page_get_space_id(block->page.frame),
                  page_get_page_no(block->page.frame)) == block->page.id());
}

#ifdef UNIV_DEBUG
/** Check if we are holding an rw-latch in this mini-transaction
@param lock   latch to search for
@param type   held latch type
@return whether (lock,type) is contained */
bool mtr_t::memo_contains(const index_lock &lock, mtr_memo_type_t type) const
{
  ut_ad(type == MTR_MEMO_X_LOCK || type == MTR_MEMO_S_LOCK ||
        type == MTR_MEMO_SX_LOCK);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &lock && slot.type == type)
    {
      switch (type) {
      case MTR_MEMO_X_LOCK:
        ut_ad(lock.have_x());
        break;
      case MTR_MEMO_SX_LOCK:
        ut_ad(lock.have_u_or_x());
        break;
      case MTR_MEMO_S_LOCK:
        ut_ad(lock.have_s());
        break;
      default:
        break;
      }
      return true;
    }
  }

  return false;
}

/** Check if memo contains the given item.
@param object		object to search
@param flags		specify types of object (can be ORred) of
			MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */
bool mtr_t::memo_contains_flagged(const void *object, ulint flags) const
{
  ut_ad(is_active());
  ut_ad(flags);
  /* Look for rw-lock-related and page-related flags. */
  ut_ad(!(flags & ulint(~(MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_X_FIX |
                          MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_BUF_FIX |
                          MTR_MEMO_MODIFY | MTR_MEMO_X_LOCK |
                          MTR_MEMO_SX_LOCK | MTR_MEMO_S_LOCK))));
  /* Either some rw-lock-related or page-related flags
  must be specified, but not both at the same time. */
  ut_ad(!(flags & (MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_X_FIX |
                   MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_BUF_FIX |
                   MTR_MEMO_MODIFY)) ==
        !!(flags & (MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK | MTR_MEMO_S_LOCK)));

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (object != slot.object)
      continue;

    auto f = flags & slot.type;
    if (!f)
      continue;

    if (f & (MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX))
    {
      const block_lock &lock= static_cast<const buf_page_t*>(object)->lock;
      ut_ad(!(f & MTR_MEMO_PAGE_S_FIX) || lock.have_s());
      ut_ad(!(f & MTR_MEMO_PAGE_SX_FIX) || lock.have_u_or_x());
      ut_ad(!(f & MTR_MEMO_PAGE_X_FIX) || lock.have_x());
    }
    else
    {
      const index_lock &lock= *static_cast<const index_lock*>(object);
      ut_ad(!(f & MTR_MEMO_S_LOCK) || lock.have_s());
      ut_ad(!(f & MTR_MEMO_SX_LOCK) || lock.have_u_or_x());
      ut_ad(!(f & MTR_MEMO_X_LOCK) || lock.have_x());
    }

    return true;
  }

  return false;
}

buf_block_t* mtr_t::memo_contains_page_flagged(const byte *ptr, ulint flags)
  const
{
  ptr= page_align(ptr);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    ut_ad(slot.object);
    if (!(flags & slot.type))
      continue;

    buf_page_t *bpage= static_cast<buf_page_t*>(slot.object);

    if (ptr != bpage->frame)
      continue;

    ut_ad(!(slot.type & MTR_MEMO_PAGE_S_FIX) || bpage->lock.have_s());
    ut_ad(!(slot.type & MTR_MEMO_PAGE_SX_FIX) || bpage->lock.have_u_or_x());
    ut_ad(!(slot.type & MTR_MEMO_PAGE_X_FIX) || bpage->lock.have_x());
    return static_cast<buf_block_t*>(slot.object);
  }

  return nullptr;
}
#endif /* UNIV_DEBUG */


/** Mark the given latched page as modified.
@param block   page that will be modified */
void mtr_t::set_modified(const buf_block_t &block)
{
  if (block.page.id().space() >= SRV_TMP_SPACE_ID)
  {
    const_cast<buf_block_t&>(block).page.set_temp_modified();
    return;
  }

  m_modifications= true;

  if (UNIV_UNLIKELY(m_log_mode == MTR_LOG_NONE))
    return;

  for (mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &block &&
        slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX))
    {
      if (slot.type & MTR_MEMO_MODIFY)
        ut_ad(m_made_dirty || block.page.oldest_modification() > 1);
      else
      {
        slot.type= static_cast<mtr_memo_type_t>(slot.type | MTR_MEMO_MODIFY);
        if (!m_made_dirty)
          m_made_dirty= block.page.oldest_modification() <= 1;
      }
      return;
    }
  }

  /* This must be PageConverter::update_page() in IMPORT TABLESPACE. */
  ut_ad(m_memo.empty());
  ut_ad(!block.page.in_LRU_list);
}

void mtr_t::init(buf_block_t *b)
{
  const page_id_t id{b->page.id()};
  ut_ad(is_named_space(id.space()));
  ut_ad(!m_freed_pages == !m_freed_space);
  ut_ad(memo_contains_flagged(b, MTR_MEMO_PAGE_X_FIX));

  if (id.space() >= SRV_TMP_SPACE_ID)
    b->page.set_temp_modified();
  else
  {
    for (mtr_memo_slot_t &slot : m_memo)
    {
      if (slot.object == b && slot.type & MTR_MEMO_PAGE_X_FIX)
      {
        slot.type= MTR_MEMO_PAGE_X_MODIFY;
        m_modifications= true;
        if (!m_made_dirty)
          m_made_dirty= b->page.oldest_modification() <= 1;
        goto found;
      }
    }
    ut_ad("block not X-latched" == 0);
  }

 found:
  if (UNIV_LIKELY_NULL(m_freed_space) &&
      m_freed_space->id == id.space() &&
      m_freed_pages->remove_if_exists(id.page_no()) &&
      m_freed_pages->empty())
  {
    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
  }

  b->page.set_reinit(b->page.state() & buf_page_t::LRU_MASK);

  if (!is_logged())
    return;

  m_log.close(log_write<INIT_PAGE>(id, &b->page));
  m_last_offset= FIL_PAGE_TYPE;
}

/** Free a page.
@param space   tablespace
@param offset  offset of the page to be freed */
void mtr_t::free(const fil_space_t &space, uint32_t offset)
{
  ut_ad(is_named_space(&space));
  ut_ad(!m_freed_space || m_freed_space == &space);

  if (is_logged())
  {
    buf_block_t *freed= nullptr;
    const page_id_t id{space.id, offset};

    for (auto it= m_memo.end(); it != m_memo.begin(); )
    {
      it--;
    next:
      mtr_memo_slot_t &slot= *it;
      buf_block_t *block= static_cast<buf_block_t*>(slot.object);
      ut_ad(block);
      if (block == freed)
      {
        if (slot.type & (MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX))
          slot.type= MTR_MEMO_PAGE_X_FIX;
        else
        {
          ut_ad(slot.type == MTR_MEMO_BUF_FIX);
          block->page.unfix();
          m_memo.erase(it, it + 1);
          goto next;
        }
      }
      else if (slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX) &&
               block->page.id() == id)
      {
        ut_ad(!block->page.is_freed());
        ut_ad(!freed);
        freed= block;
        if (!(slot.type & MTR_MEMO_PAGE_X_FIX))
        {
          ut_d(bool upgraded=) block->page.lock.x_lock_upgraded();
          ut_ad(upgraded);
        }
        if (id.space() >= SRV_TMP_SPACE_ID)
        {
          block->page.set_temp_modified();
          slot.type= MTR_MEMO_PAGE_X_FIX;
        }
        else
        {
          slot.type= MTR_MEMO_PAGE_X_MODIFY;
          if (!m_made_dirty)
            m_made_dirty= block->page.oldest_modification() <= 1;
        }
#ifdef BTR_CUR_HASH_ADAPT
        if (block->index)
          btr_search_drop_page_hash_index(block, false);
#endif /* BTR_CUR_HASH_ADAPT */
        block->page.set_freed(block->page.state());
      }
    }

    m_log.close(log_write<FREE_PAGE>(id, nullptr));
  }
}

void small_vector_base::grow_by_1(void *small, size_t element_size)
{
  const size_t cap= Capacity*= 2, s= cap * element_size;
  void *new_begin;
  if (BeginX == small)
  {
    new_begin= my_malloc(PSI_NOT_INSTRUMENTED, s, MYF(0));
    memcpy(new_begin, BeginX, size() * element_size);
    TRASH_FREE(small, size() * element_size);
  }
  else
    new_begin= my_realloc(PSI_NOT_INSTRUMENTED, BeginX, s, MYF(0));

  BeginX= new_begin;
}
