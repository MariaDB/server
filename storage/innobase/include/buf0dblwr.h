/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

  /** the page number of the first doublewrite block (block_size() pages) */
  page_id_t block1= page_id_t(0, 0);
  /** the page number of the second doublewrite block (block_size() pages) */
  page_id_t block2= page_id_t(0, 0);

  /** mutex protecting the data members below */
  mysql_mutex_t mutex;
  /** condition variable for !batch_running */
  pthread_cond_t cond;
  /** whether a batch is being written from the doublewrite buffer */
  bool batch_running;
  /** number of expected flush_buffered_writes_completed() calls */
  unsigned flushing_buffered_writes;
  /** pages submitted to flush_buffered_writes() */
  ulint pages_submitted;
  /** number of flush_buffered_writes_completed() calls */
  ulint writes_completed;
  /** number of pages written by flush_buffered_writes_completed() */
  ulint pages_written;

  slot slots[2];
  slot *active_slot= &slots[0];

  /** Initialize the doublewrite buffer data structure.
  @param header   doublewrite page header in the TRX_SYS page */
  inline void init(const byte *header);

  /** Flush possible buffered writes to persistent storage. */
  bool flush_buffered_writes(const ulint size);

public:
  /** Create or restore the doublewrite buffer in the TRX_SYS page.
  @return whether the operation succeeded */
  bool create();
  /** Free the doublewrite buffer. */
  void close();

  /** Acquire the mutex */
  void lock() { mysql_mutex_lock(&mutex); }
  /** @return the number of submitted page writes */
  ulint submitted() const
  { mysql_mutex_assert_owner(&mutex); return pages_submitted; }
  /** @return the number of completed batches */
  ulint batches() const
  { mysql_mutex_assert_owner(&mutex); return writes_completed; }
  /** @return the number of final pages written */
  ulint written() const
  { mysql_mutex_assert_owner(&mutex); return pages_written; }
  /** Release the mutex */
  void unlock() { mysql_mutex_unlock(&mutex); }

  /** Initialize the doublewrite buffer memory structure on recovery.
  If we are upgrading from a version before MySQL 4.1, then this
  function performs the necessary update operations to support
  innodb_file_per_table. If we are in a crash recovery, this function
  loads the pages from double write buffer into memory.
  @param file File handle
  @param path Path name of file
  @return DB_SUCCESS or error code */
  dberr_t init_or_load_pages(pfs_os_file_t file, const char *path);

  /** Process and remove the double write buffer pages for all tablespaces. */
  void recover();

  /** Update the doublewrite buffer on data page write completion. */
  void write_completed();
  /** Flush possible buffered writes to persistent storage.
  It is very important to call this function after a batch of writes has been
  posted, and also when we may have to wait for a page latch!
  Otherwise a deadlock of threads can occur. */
  void flush_buffered_writes();
  /** Update the doublewrite buffer on write batch completion
  @param request  the completed batch write request */
  void flush_buffered_writes_completed(const IORequest &request);

  /** Size of the doublewrite block in pages */
  uint32_t block_size() const { return FSP_EXTENT_SIZE; }

  /** Schedule a page write. If the doublewrite memory buffer is full,
  flush_buffered_writes() will be invoked to make space.
  @param request    asynchronous write request
  @param size       payload size in bytes */
  void add_to_batch(const IORequest &request, size_t size);

  /** Determine whether the doublewrite buffer is initialized */
  bool is_initialised() const
  { return UNIV_LIKELY(block1 != page_id_t(0, 0)); }

  /** @return whether a page identifier is part of the doublewrite buffer */
  bool is_inside(const page_id_t id) const
  {
    if (!is_initialised())
      return false;
    ut_ad(block1 < block2);
    if (id < block1)
      return false;
    const uint32_t size= block_size();
    return id < block1 + size || (id >= block2 && id < block2 + size);
  }

  /** Wait for flush_buffered_writes() to be fully completed */
  void wait_flush_buffered_writes()
  {
    if (is_initialised())
    {
      mysql_mutex_lock(&mutex);
      while (batch_running)
        my_cond_wait(&cond, &mutex.m_mutex);
      mysql_mutex_unlock(&mutex);
    }
  }
};

/** The doublewrite buffer */
extern buf_dblwr_t buf_dblwr;
