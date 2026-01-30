/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/buf0dblwr.h
Doublewrite buffer module

Created 2011/12/19 Inaam Rana
*******************************************************/

#pragma once

#include "os0file.h"
#include "buf0types.h"

/** Doublewrite control struct */
class buf_dblwr_t
{
  struct element
  {
    /** asynchronous write request */
    IORequest request;
    /** payload size in bytes */
    size_t size;
  };

  struct slot
  {
    /** first free position in write_buf measured in units of
     * srv_page_size */
    ulint first_free;
    /** number of slots reserved for the current write batch */
    ulint reserved;
    /** the doublewrite buffer, aligned to srv_page_size */
    byte* write_buf;
    /** buffer blocks to be written via write_buf */
    element* buf_block_arr;
  };

  /** the page number of the first doublewrite block (block_size pages) */
  page_id_t block1{0, 0};
  /** the page number of the second doublewrite block (block_size pages) */
  page_id_t block2{0, 0};

  /** mutex protecting the data members below */
  mysql_mutex_t mutex;
  /** condition variable for !batch_running */
  pthread_cond_t cond;
  /** whether a batch is being written from the doublewrite buffer */
  bool batch_running;
  /** number of expected flush_buffered_writes_completed() calls */
  unsigned flushing_buffered_writes;
  /** number of flush_buffered_writes_completed() calls */
  ulint writes_completed;
  /** number of pages written by flush_buffered_writes_completed() */
  ulint pages_written;

  slot slots[2];
  slot *active_slot;

  /** Size of the doublewrite block in pages */
  uint32_t block_size;

public:
  /** Values of use */
  enum usage {
    /** Assume that writes are atomic */
    USE_NO= 0,
    /** Use the doublewrite buffer with full durability */
    USE_YES,
    /** Durable writes to the doublewrite buffer, not to data files */
    USE_FAST
  };
  /** The value of innodb_doublewrite */
  ulong use;
private:
  /** Initialise the persistent storage of the doublewrite buffer.
  @param header   doublewrite page header in the TRX_SYS page */
  inline void init(const byte *header) noexcept;

  /** Flush possible buffered writes to persistent storage. */
  bool flush_buffered_writes(const ulint size) noexcept;

public:
  /** Initialise the doublewrite buffer data structures. */
  void init() noexcept;
  /** Create or restore the doublewrite buffer in the TRX_SYS page.
  @return whether the operation succeeded */
  bool create() noexcept;
  /** Free the doublewrite buffer. */
  void close() noexcept;

  /** Acquire the mutex */
  void lock() noexcept { mysql_mutex_lock(&mutex); }
  /** @return the number of completed batches */
  ulint batches() const noexcept
  { mysql_mutex_assert_owner(&mutex); return writes_completed; }
  /** @return the number of override final pages written */
  ulint written() const noexcept
  { mysql_mutex_assert_owner(&mutex); return pages_written; }
  /** Release the mutex */
  void unlock() noexcept { mysql_mutex_unlock(&mutex); }

  /** Initialize the doublewrite buffer memory structure on recovery.
  If we are upgrading from a version before MySQL 4.1, then this
  function performs the necessary update operations to support
  innodb_file_per_table. If we are in a crash recovery, this function
  loads the pages from double write buffer which are not older than
  the checkpoint into memory.
  @param file File handle
  @param path Path name of file
  @return DB_SUCCESS or error code */
  dberr_t init_or_load_pages(pfs_os_file_t file, const char *path) noexcept;

  /** Process and remove the double write buffer pages for all tablespaces. */
  void recover() noexcept;

  /** Update the doublewrite buffer on data page write completion. */
  void write_completed() noexcept;
  /** Flush possible buffered writes to persistent storage.
  It is very important to call this function after a batch of writes has been
  posted, and also when we may have to wait for a page latch!
  Otherwise a deadlock of threads can occur. */
  void flush_buffered_writes() noexcept;
  /** Update the doublewrite buffer on write batch completion
  @param request  the completed batch write request */
  void flush_buffered_writes_completed(const IORequest &request) noexcept;

  /** Schedule a page write. If the doublewrite memory buffer is full,
  flush_buffered_writes() will be invoked to make space.
  @param request    asynchronous write request
  @param size       payload size in bytes */
  void add_to_batch(const IORequest &request, size_t size) noexcept;

  /** Determine whether the doublewrite buffer has been created */
  bool is_created() const noexcept
  { return UNIV_LIKELY(block1 != page_id_t(0, 0)); }

  /** @return whether the doublewrite buffer is in use */
  bool in_use() const { return is_created() && use; }
  /** @return whether fsync() is needed on non-doublewrite pages */
  bool need_fsync() const { return use < USE_FAST; }

  void set_use(ulong use)
  {
    ut_ad(use <= USE_FAST);
    mysql_mutex_lock(&mutex);
    this->use= use;
    mysql_mutex_unlock(&mutex);
  }

  /** @return whether a page identifier is part of the doublewrite buffer */
  bool is_inside(const page_id_t id) const noexcept
  {
    if (!is_created())
      return false;
    ut_ad(block1 < block2);
    if (id < block1)
      return false;
    return id < block1 + block_size ||
      (id >= block2 && id < block2 + block_size);
  }

  /** Wait for flush_buffered_writes() to be fully completed */
  void wait_flush_buffered_writes() noexcept
  {
    mysql_mutex_lock(&mutex);
    while (batch_running)
      my_cond_wait(&cond, &mutex.m_mutex);
    mysql_mutex_unlock(&mutex);
  }

  /** Print double write state information. */
  ATTRIBUTE_COLD void print_info() const noexcept;
};

/** The doublewrite buffer */
extern buf_dblwr_t buf_dblwr;
