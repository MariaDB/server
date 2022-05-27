/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2014, 2022, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <debug_sync.h>
#include <my_service_manager.h>

#include "log0log.h"
#include "log0crypt.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0stats_bg.h"
#include "btr0defragment.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "buf0dump.h"
#include "log0sync.h"
#include "log.h"

/*
General philosophy of InnoDB redo-logs:

Every change to a contents of a data page must be done
through mtr_t, and mtr_t::commit() will write log records
to the InnoDB redo log. */

alignas(CPU_LEVEL1_DCACHE_LINESIZE)
static group_commit_lock flush_lock;
alignas(CPU_LEVEL1_DCACHE_LINESIZE)
static group_commit_lock write_lock;

/** Redo log system */
log_t	log_sys;

/* Margins for free space in the log buffer after a log entry is catenated */
#define LOG_BUF_FLUSH_RATIO	2
#define LOG_BUF_FLUSH_MARGIN	((4 * 4096) /* cf. log_t::append_prepare() */ \
				 + (4U << srv_page_size_shift))

void log_t::set_capacity()
{
#ifndef SUX_LOCK_GENERIC
	ut_ad(log_sys.latch.is_write_locked());
#endif
	/* Margin for the free space in the smallest log, before a new query
	step which modifies the database, is started */

	lsn_t smallest_capacity = srv_log_file_size - log_t::START_OFFSET;
	/* Add extra safety */
	smallest_capacity -= smallest_capacity / 10;

	lsn_t margin = smallest_capacity - (48 << srv_page_size_shift);
	margin -= margin / 10;	/* Add still some extra safety */

	log_sys.log_capacity = smallest_capacity;

	log_sys.max_modified_age_async = margin - margin / 8;
	log_sys.max_checkpoint_age = margin;
}

/** Initialize the redo log subsystem. */
void log_t::create()
{
  ut_ad(this == &log_sys);
  ut_ad(!is_initialised());

  latch.SRW_LOCK_INIT(log_latch_key);
  init_lsn_lock();

  /* LSN 0 and 1 are reserved; @see buf_page_t::oldest_modification_ */
  lsn.store(FIRST_LSN, std::memory_order_relaxed);
  flushed_to_disk_lsn.store(FIRST_LSN, std::memory_order_relaxed);
  write_lsn= FIRST_LSN;

#ifndef HAVE_PMEM
  buf= static_cast<byte*>(ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME));
  TRASH_ALLOC(buf, buf_size);
  flush_buf= static_cast<byte*>(ut_malloc_dontdump(buf_size,
                                                   PSI_INSTRUMENT_ME));
  TRASH_ALLOC(flush_buf, buf_size);
  checkpoint_buf= static_cast<byte*>(aligned_malloc(4096, 4096));
  memset_aligned<4096>(checkpoint_buf, 0, 4096);
#else
  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
#endif

  max_buf_free= buf_size / LOG_BUF_FLUSH_RATIO - LOG_BUF_FLUSH_MARGIN;
  set_check_flush_or_checkpoint();

  last_checkpoint_lsn= FIRST_LSN;
  log_capacity= 0;
  max_modified_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_lsn= 0;
  checkpoint_pending= false;

  buf_free= 0;

  ut_ad(is_initialised());
}

dberr_t log_file_t::close() noexcept
{
  ut_a(is_opened());

  if (!os_file_close_func(m_file))
    return DB_ERROR;

  m_file= OS_FILE_CLOSED;
  return DB_SUCCESS;
}

dberr_t log_file_t::read(os_offset_t offset, span<byte> buf) noexcept
{
  ut_ad(is_opened());
  return os_file_read(IORequestRead, m_file, buf.data(), offset, buf.size());
}

void log_file_t::write(os_offset_t offset, span<const byte> buf) noexcept
{
  ut_ad(is_opened());
  if (dberr_t err= os_file_write_func(IORequestWrite, "ib_logfile0", m_file,
                                      buf.data(), offset, buf.size()))
    ib::fatal() << "write(\"ib_logfile0\") returned " << err;
}

#ifdef HAVE_PMEM
# include <libpmem.h>

/** Attempt to memory map a file.
@param file  log file handle
@param size  file size
@return pointer to memory mapping
@retval MAP_FAILED  if the memory cannot be mapped */
static void *log_mmap(os_file_t file, os_offset_t size)
{
  void *ptr=
    my_mmap(0, size_t(size),
            srv_read_only_mode ? PROT_READ : PROT_READ | PROT_WRITE,
            MAP_SHARED_VALIDATE | MAP_SYNC, file, 0);
#ifdef __linux__
  if (ptr == MAP_FAILED)
  {
    struct stat st;
    if (!fstat(file, &st))
    {
      MSAN_STAT_WORKAROUND(&st);
      const auto st_dev= st.st_dev;
      if (!stat("/dev/shm", &st))
      {
        MSAN_STAT_WORKAROUND(&st);
        if (st.st_dev == st_dev)
          ptr= my_mmap(0, size_t(size),
                       srv_read_only_mode ? PROT_READ : PROT_READ | PROT_WRITE,
                       MAP_SHARED, file, 0);
      }
    }
  }
#endif /* __linux__ */
  return ptr;
}
#endif

void log_t::attach(log_file_t file, os_offset_t size)
{
  log= file;
  ut_ad(!size || size >= START_OFFSET + SIZE_OF_FILE_CHECKPOINT);
  file_size= size;

#ifdef HAVE_PMEM
  ut_ad(!buf);
  ut_ad(!flush_buf);
  if (size && !(size_t(size) & 4095))
  {
    void *ptr= log_mmap(log.m_file, size);
    if (ptr != MAP_FAILED)
    {
      log.close();
      mprotect(ptr, size_t(size), PROT_READ);
      buf= static_cast<byte*>(ptr);
#if defined __linux__ || defined _WIN32
      set_block_size(CPU_LEVEL1_DCACHE_LINESIZE);
#endif
      return;
    }
  }
  buf= static_cast<byte*>(ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME));
  TRASH_ALLOC(buf, buf_size);
  flush_buf= static_cast<byte*>(ut_malloc_dontdump(buf_size,
                                                   PSI_INSTRUMENT_ME));
  TRASH_ALLOC(flush_buf, buf_size);
#endif

#if defined __linux__ || defined _WIN32
  if (!block_size)
    set_block_size(512);
# ifdef __linux__
  else if (srv_file_flush_method != SRV_O_DSYNC &&
           srv_file_flush_method != SRV_O_DIRECT &&
           srv_file_flush_method != SRV_O_DIRECT_NO_FSYNC)
    sql_print_information("InnoDB: Buffered log writes (block size=%u bytes)",
                          block_size);
#endif
  else
    sql_print_information("InnoDB: File system buffers for log"
                          " disabled (block size=%u bytes)", block_size);
#endif

#ifdef HAVE_PMEM
  checkpoint_buf= static_cast<byte*>(aligned_malloc(block_size, block_size));
  memset_aligned<64>(checkpoint_buf, 0, block_size);
#endif
}

/** Write a log file header.
@param buf        log header buffer
@param lsn        log sequence number corresponding to log_sys.START_OFFSET
@param encrypted  whether the log is encrypted */
void log_t::header_write(byte *buf, lsn_t lsn, bool encrypted)
{
  mach_write_to_4(my_assume_aligned<4>(buf) + LOG_HEADER_FORMAT,
                  log_sys.FORMAT_10_8);
  mach_write_to_8(my_assume_aligned<8>(buf + LOG_HEADER_START_LSN), lsn);
  static constexpr const char LOG_HEADER_CREATOR_CURRENT[]=
    "MariaDB "
    IB_TO_STR(MYSQL_VERSION_MAJOR) "."
    IB_TO_STR(MYSQL_VERSION_MINOR) "."
    IB_TO_STR(MYSQL_VERSION_PATCH);

  strcpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
         LOG_HEADER_CREATOR_CURRENT);
  static_assert(LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR >=
                sizeof LOG_HEADER_CREATOR_CURRENT, "compatibility");
  if (encrypted)
    log_crypt_write_header(buf + LOG_HEADER_CREATOR_END);
  mach_write_to_4(my_assume_aligned<4>(508 + buf), my_crc32c(0, buf, 508));
}

void log_t::create(lsn_t lsn) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
  ut_ad(!recv_no_log_write);
  ut_ad(is_latest());
  ut_ad(this == &log_sys);

  this->lsn.store(lsn, std::memory_order_relaxed);
  this->flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
  first_lsn= lsn;
  write_lsn= lsn;

  last_checkpoint_lsn= 0;

#ifdef HAVE_PMEM
  if (is_pmem())
  {
    mprotect(buf, size_t(file_size), PROT_READ | PROT_WRITE);
    memset_aligned<4096>(buf, 0, 4096);
    buf_free= START_OFFSET;
  }
  else
#endif
  {
    buf_free= 0;
    memset_aligned<4096>(flush_buf, 0, buf_size);
    memset_aligned<4096>(buf, 0, buf_size);
  }

  log_sys.header_write(buf, lsn, is_encrypted());
  DBUG_PRINT("ib_log", ("write header " LSN_PF, lsn));

#ifdef HAVE_PMEM
  if (is_pmem())
    pmem_persist(buf, 512);
  else
#endif
  {
    log.write(0, {buf, 4096});
    memset_aligned<512>(buf, 0, 512);
  }
}

void log_t::close_file()
{
#ifdef HAVE_PMEM
  if (is_pmem())
  {
    ut_ad(!is_opened());
    ut_ad(!checkpoint_buf);
    if (buf)
    {
      my_munmap(buf, file_size);
      buf= nullptr;
    }
    return;
  }

  ut_free_dodump(buf, buf_size);
  buf= nullptr;
  ut_free_dodump(flush_buf, buf_size);
  flush_buf= nullptr;
  aligned_free(checkpoint_buf);
  checkpoint_buf= nullptr;
#endif
  if (is_opened())
    if (const dberr_t err= log.close())
      ib::fatal() << "closing ib_logfile0 failed: " << err;
}

/** Acquire the latches that protect log resizing. */
static void log_resize_acquire()
{
  if (!log_sys.is_pmem())
  {
    while (flush_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
           group_commit_lock::ACQUIRED);
    while (write_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
           group_commit_lock::ACQUIRED);
  }

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
}

/** Release the latches that protect log resizing. */
void log_resize_release()
{
  log_sys.latch.wr_unlock();

  if (!log_sys.is_pmem())
  {
    lsn_t lsn1= write_lock.release(write_lock.value());
    lsn_t lsn2= flush_lock.release(flush_lock.value());
    if (lsn1 || lsn2)
      log_write_up_to(std::max(lsn1, lsn2), true, nullptr);
  }
}

/** Start resizing the log and release the exclusive latch.
@param size  requested new file_size
@return whether the resizing was started successfully */
log_t::resize_start_status log_t::resize_start(os_offset_t size) noexcept
{
  ut_ad(size >= 4U << 20);
  ut_ad(!(size & 4095));
  ut_ad(!srv_read_only_mode);

  log_resize_acquire();

  resize_start_status status= RESIZE_NO_CHANGE;
  lsn_t start_lsn{0};

  if (resize_in_progress())
    status= RESIZE_IN_PROGRESS;
  else if (size != file_size)
  {
    ut_ad(!resize_in_progress());
    ut_ad(!resize_log.is_opened());
    ut_ad(!resize_buf);
    ut_ad(!resize_flush_buf);
    std::string path{get_log_file_path("ib_logfile101")};
    bool success;
    resize_lsn.store(1, std::memory_order_relaxed);
    resize_target= 0;
    resize_log.m_file=
      os_file_create_func(path.c_str(),
                          OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT,
                          OS_FILE_NORMAL, OS_LOG_FILE, false, &success);
    if (success)
    {
      log_resize_release();

      void *ptr= nullptr, *ptr2= nullptr;
      success= os_file_set_size(path.c_str(), resize_log.m_file, size);
      if (!success);
#ifdef HAVE_PMEM
      else if (is_pmem())
      {
        ptr= log_mmap(resize_log.m_file, size);
        if (ptr == MAP_FAILED)
          goto alloc_fail;
      }
#endif
      else
      {
        ptr= ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME);
        if (ptr)
        {
          TRASH_ALLOC(ptr, buf_size);
          ptr2= ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME);
          if (ptr2)
            TRASH_ALLOC(ptr2, buf_size);
          else
          {
            ut_free_dodump(ptr, buf_size);
            ptr= nullptr;
            goto alloc_fail;
          }
        }
        else
        alloc_fail:
          success= false;
      }

      log_resize_acquire();

      if (!success)
      {
        resize_log.close();
        IF_WIN(DeleteFile(path.c_str()), unlink(path.c_str()));
      }
      else
      {
        resize_target= size;
        resize_buf= static_cast<byte*>(ptr);
        resize_flush_buf= static_cast<byte*>(ptr2);
        if (is_pmem())
        {
          resize_log.close();
          start_lsn= get_lsn();
        }
        else
        {
          memcpy_aligned<16>(resize_buf, buf, (buf_free + 15) & ~15);
          start_lsn= first_lsn +
            (~lsn_t{get_block_size() - 1} & (write_lsn - first_lsn));
        }
      }
      resize_lsn.store(start_lsn, std::memory_order_relaxed);
      status= success ? RESIZE_STARTED : RESIZE_FAILED;
    }
  }

  log_resize_release();

  if (start_lsn)
    buf_flush_ahead(start_lsn, false);

  return status;
}

/** Abort log resizing. */
void log_t::resize_abort() noexcept
{
  log_resize_acquire();

  if (resize_in_progress() > 1)
  {
    if (!is_pmem())
    {
      resize_log.close();
      ut_free_dodump(resize_buf, buf_size);
      ut_free_dodump(resize_flush_buf, buf_size);
      resize_flush_buf= nullptr;
    }
#ifdef HAVE_PMEM
    else
    {
      ut_ad(!resize_log.is_opened());
      ut_ad(!resize_flush_buf);
      if (resize_buf)
        my_munmap(resize_buf, resize_target);
    }
#endif
    resize_buf= nullptr;
    resize_target= 0;
    resize_lsn.store(0, std::memory_order_relaxed);
  }

  log_resize_release();
}

/** Write an aligned buffer to ib_logfile0.
@param buf    buffer to be written
@param len    length of data to be written
@param offset log file offset */
static void log_write_buf(const byte *buf, size_t len, lsn_t offset)
{
  ut_ad(write_lock.is_owner());
  ut_ad(!recv_no_log_write);
  ut_d(const size_t block_size_1= log_sys.get_block_size() - 1);
  ut_ad(!(offset & block_size_1));
  ut_ad(!(len & block_size_1));
  ut_ad(!(size_t(buf) & block_size_1));
  ut_ad(len);

  if (UNIV_LIKELY(offset + len <= log_sys.file_size))
  {
write:
    log_sys.log.write(offset, {buf, len});
    return;
  }

  const size_t write_len= size_t(log_sys.file_size - offset);
  log_sys.log.write(offset, {buf, write_len});
  len-= write_len;
  buf+= write_len;
  ut_ad(log_sys.START_OFFSET + len < offset);
  offset= log_sys.START_OFFSET;
  goto write;
}

/** Invoke commit_checkpoint_notify_ha() to notify that outstanding
log writes have been completed. */
void log_flush_notify(lsn_t flush_lsn);

#if 0 // Currently we overwrite the last log block until it is complete.
/** CRC-32C of pad messages using between 1 and 15 bytes of NUL bytes
in the payload */
static const unsigned char pad_crc[15][4]= {
  {0xA6,0x59,0xC1,0xDB}, {0xF2,0xAF,0x80,0x73}, {0xED,0x02,0xF1,0x90},
  {0x68,0x4E,0xA3,0xF3}, {0x5D,0x1B,0xEA,0x6A}, {0xE0,0x01,0x86,0xB9},
  {0xD1,0x06,0x86,0xF5}, {0xEB,0x20,0x12,0x33}, {0xBA,0x73,0xB2,0xA3},
  {0x5F,0xA2,0x08,0x03}, {0x70,0x03,0xD6,0x9D}, {0xED,0xB3,0x49,0x78},
  {0xFD,0xD6,0xB9,0x9C}, {0x25,0xF8,0xB1,0x2C}, {0xCD,0xAA,0xE7,0x10}
};

/** Pad the log with some dummy bytes
@param lsn    desired log sequence number
@param pad    number of bytes to append to the log
@param begin  buffer to write 'pad' bytes to
@param extra  buffer for additional pad bytes (up to 15 bytes)
@return additional bytes used in extra[] */
ATTRIBUTE_NOINLINE
static size_t log_pad(lsn_t lsn, size_t pad, byte *begin, byte *extra)
{
  ut_ad(!(size_t(begin + pad) & (log_sys.get_block_size() - 1)));
  byte *b= begin;
  const byte seq{log_sys.get_sequence_bit(lsn)};
  /* The caller should never request padding such that the
  file would wrap around to the beginning. That is, the sequence
  bit must be the same for all records. */
  ut_ad(seq == log_sys.get_sequence_bit(lsn + pad));

  if (log_sys.is_encrypted())
  {
    /* The lengths of our pad messages vary between 15 and 29 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum, 8 NUL bytes nonce). */
    if (pad < 15)
    {
      extra[0]= FILE_CHECKPOINT | 1;
      extra[1]= 0;
      extra[2]= seq;
      memcpy(extra + 3, pad_crc[0], 4);
      memset(extra + 7, 0, 8);
      memcpy(b, extra, pad);
      memmove(extra, extra + pad, 15 - pad);
      return 15 - pad;
    }

    /* Pad first with 29-byte messages until the remaining size is
    less than 29+15 bytes, and then write 1 or 2 shorter messages. */
    const byte *const end= begin + pad;
    for (; b + (29 + 15) < end; b+= 29)
    {
      b[0]= FILE_CHECKPOINT | 15;
      memset(b + 1, 0, 15);
      b[16]= seq;
      memcpy(b + 17, pad_crc[14], 4);
      memset(b + 21, 0, 8);
    }
    if (b + 29 < end)
    {
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= seq;
      memcpy(b + 3, pad_crc[0], 4);
      memset(b + 7, 0, 8);
      b+= 15;
    }
    const size_t last_pad(end - b);
    ut_ad(last_pad >= 15);
    ut_ad(last_pad <= 29);
    b[0]= FILE_CHECKPOINT | byte(last_pad - 14);
    memset(b + 1, 0, last_pad - 14);
    b[last_pad - 13]= seq;
    memcpy(b + last_pad - 12, pad_crc[last_pad - 15], 4);
    memset(b + last_pad - 8, 0, 8);
  }
  else
  {
    /* The lengths of our pad messages vary between 7 and 21 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum). */
    if (pad < 7)
    {
      extra[0]= FILE_CHECKPOINT | 1;
      extra[1]= 0;
      extra[2]= seq;
      memcpy(extra + 3, pad_crc[0], 4);
      memcpy(b, extra, pad);
      memmove(extra, extra + pad, 7 - pad);
      return 7 - pad;
    }

    /* Pad first with 21-byte messages until the remaining size is
    less than 21+7 bytes, and then write 1 or 2 shorter messages. */
    const byte *const end= begin + pad;
    for (; b + (21 + 7) < end; b+= 21)
    {
      b[0]= FILE_CHECKPOINT | 15;
      memset(b + 1, 0, 15);
      b[16]= seq;
      memcpy(b + 17, pad_crc[14], 4);
    }
    if (b + 21 < end)
    {
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= seq;
      memcpy(b + 3, pad_crc[0], 4);
      b+= 7;
    }
    const size_t last_pad(end - b);
    ut_ad(last_pad >= 7);
    ut_ad(last_pad <= 21);
    b[0]= FILE_CHECKPOINT | byte(last_pad - 6);
    memset(b + 1, 0, last_pad - 6);
    b[last_pad - 5]= seq;
    memcpy(b + last_pad - 4, pad_crc[last_pad - 7], 4);
  }

  return 0;
}
#endif

#ifdef HAVE_PMEM
/** Persist the log.
@param lsn    desired new value of flushed_to_disk_lsn */
inline void log_t::persist(lsn_t lsn) noexcept
{
  ut_ad(is_pmem());
  ut_ad(!write_lock.is_owner());
  ut_ad(!flush_lock.is_owner());

  lsn_t old= flushed_to_disk_lsn.load(std::memory_order_relaxed);

  if (old >= lsn)
    return;

  const lsn_t resizing{resize_in_progress()};
  if (UNIV_UNLIKELY(resizing))
    latch.rd_lock(SRW_LOCK_CALL);
  const size_t start(calc_lsn_offset(old));
  const size_t end(calc_lsn_offset(lsn));

  if (UNIV_UNLIKELY(end < start))
  {
    pmem_persist(log_sys.buf + start, log_sys.file_size - start);
    pmem_persist(log_sys.buf + log_sys.START_OFFSET,
                 end - log_sys.START_OFFSET);
  }
  else
    pmem_persist(log_sys.buf + start, end - start);

  old= flushed_to_disk_lsn.load(std::memory_order_relaxed);

  if (old < lsn)
  {
    while (!flushed_to_disk_lsn.compare_exchange_weak
           (old, lsn, std::memory_order_release, std::memory_order_relaxed))
      if (old >= lsn)
        break;

    log_flush_notify(lsn);
    DBUG_EXECUTE_IF("crash_after_log_write_upto", DBUG_SUICIDE(););
  }

  if (UNIV_UNLIKELY(resizing))
    latch.rd_unlock();
}
#endif

/** Write resize_buf to resize_log.
@param length  the used length of resize_buf */
ATTRIBUTE_COLD void log_t::resize_write_buf(size_t length) noexcept
{
  const size_t block_size_1= get_block_size() - 1;
  ut_ad(!(resize_target & block_size_1));
  ut_ad(!(length & block_size_1));
  ut_ad(length > block_size_1);
  ut_ad(length <= resize_target);
  const lsn_t resizing{resize_in_progress()};
  ut_ad(resizing <= write_lsn);
  lsn_t offset= START_OFFSET +
    ((write_lsn - resizing) & ~lsn_t{block_size_1}) %
    (resize_target - START_OFFSET);

  if (UNIV_UNLIKELY(offset + length > resize_target))
  {
    offset= START_OFFSET;
    resize_lsn.store(first_lsn +
                     (~lsn_t{block_size_1} & (write_lsn - first_lsn)),
                     std::memory_order_relaxed);
  }

  ut_a(os_file_write_func(IORequestWrite, "ib_logfile101", resize_log.m_file,
                          resize_flush_buf, offset, length) == DB_SUCCESS);
}

/** Write buf to ib_logfile0.
@tparam release_latch whether to invoke latch.wr_unlock()
@return the current log sequence number */
template<bool release_latch> inline lsn_t log_t::write_buf() noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
  ut_ad(!srv_read_only_mode);
  ut_ad(!is_pmem());

  const lsn_t lsn{get_lsn(std::memory_order_relaxed)};

  if (write_lsn >= lsn)
  {
    if (release_latch)
      latch.wr_unlock();
    ut_ad(write_lsn == lsn);
  }
  else
  {
    ut_ad(!recv_no_log_write);
    write_lock.set_pending(lsn);
    ut_ad(write_lsn >= get_flushed_lsn());
    const size_t block_size_1{get_block_size() - 1};
    lsn_t offset{calc_lsn_offset(write_lsn) & ~lsn_t{block_size_1}};

    DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF " at " LSN_PF,
                          write_lsn, lsn, offset));
    const byte *write_buf{buf};
    size_t length{buf_free};
    ut_ad(length >= (calc_lsn_offset(write_lsn) & block_size_1));
    const size_t new_buf_free{length & block_size_1};
    buf_free= new_buf_free;
    ut_ad(new_buf_free == ((lsn - first_lsn) & block_size_1));

    if (new_buf_free)
    {
#if 0 /* TODO: Pad the last log block with dummy records. */
      buf_free= log_pad(lsn, get_block_size() - new_buf_free,
                        buf + new_buf_free, flush_buf);
      ... /* TODO: Update the LSN and adjust other code. */
#else
      /* The rest of the block will be written as garbage.
      (We want to avoid memset() while holding mutex.)
      This block will be overwritten later, once records beyond
      the current LSN are generated. */
# ifdef HAVE_valgrind
      MEM_MAKE_DEFINED(buf + length, get_block_size() - new_buf_free);
      if (UNIV_LIKELY_NULL(resize_flush_buf))
        MEM_MAKE_DEFINED(resize_buf + length, get_block_size() - new_buf_free);
# endif
      buf[length]= 0; /* allow recovery to catch EOF faster */
      length&= ~block_size_1;
      memcpy_aligned<16>(flush_buf, buf + length, (new_buf_free + 15) & ~15);
      if (UNIV_LIKELY_NULL(resize_flush_buf))
        memcpy_aligned<16>(resize_flush_buf, resize_buf + length,
                           (new_buf_free + 15) & ~15);
      length+= get_block_size();
#endif
    }

    std::swap(buf, flush_buf);
    std::swap(resize_buf, resize_flush_buf);
    write_to_log++;
    if (release_latch)
      latch.wr_unlock();

    if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED))
    {
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "InnoDB log write: " LSN_PF, write_lsn);
    }

    /* Do the write to the log file */
    log_write_buf(write_buf, length, offset);
    if (UNIV_LIKELY_NULL(resize_buf))
      resize_write_buf(length);
    write_lsn= lsn;
  }

  return lsn;
}

bool log_t::flush(lsn_t lsn) noexcept
{
  ut_ad(lsn >= get_flushed_lsn());
  flush_lock.set_pending(lsn);
  const bool success{srv_file_flush_method == SRV_O_DSYNC || log.flush()};
  if (UNIV_LIKELY(success))
  {
    flushed_to_disk_lsn.store(lsn, std::memory_order_release);
    log_flush_notify(lsn);
  }
  return success;
}

/** Ensure that previous log writes are durable.
@param lsn  previously written LSN
@return new durable lsn target
@retval 0  if there are no pending callbacks on flush_lock
           or there is another group commit lead.
*/
static lsn_t log_flush(lsn_t lsn)
{
  ut_ad(!log_sys.is_pmem());
  ut_a(log_sys.flush(lsn));
  DBUG_EXECUTE_IF("crash_after_log_write_upto", DBUG_SUICIDE(););
  return flush_lock.release(lsn);
}

static const completion_callback dummy_callback{[](void *) {},nullptr};

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param lsn      log sequence number that should be included in the file write
@param durable  whether the write needs to be durable
@param callback log write completion callback */
void log_write_up_to(lsn_t lsn, bool durable,
                     const completion_callback *callback)
{
  ut_ad(!srv_read_only_mode);
  ut_ad(lsn != LSN_MAX);

  if (UNIV_UNLIKELY(recv_no_ibuf_operations))
  {
    /* A non-final batch of recovery is active no writes to the log
    are allowed yet. */
    ut_a(!callback);
    return;
  }

  ut_ad(lsn <= log_sys.get_lsn());

#ifdef HAVE_PMEM
  if (log_sys.is_pmem())
  {
    ut_ad(!callback);
    if (durable)
      log_sys.persist(lsn);
    return;
  }
#endif

repeat:
  if (durable)
  {
    if (flush_lock.acquire(lsn, callback) != group_commit_lock::ACQUIRED)
      return;
    flush_lock.set_pending(log_sys.get_lsn());
  }

  lsn_t pending_write_lsn= 0, pending_flush_lsn= 0;

  if (write_lock.acquire(lsn, durable ? nullptr : callback) ==
      group_commit_lock::ACQUIRED)
  {
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    pending_write_lsn= write_lock.release(log_sys.write_buf<true>());
  }

  if (durable)
  {
    pending_flush_lsn= log_flush(write_lock.value());
  }

  if (pending_write_lsn || pending_flush_lsn)
  {
    /* There is no new group commit lead; some async waiters could stall. */
    callback= &dummy_callback;
    lsn= std::max(pending_write_lsn, pending_flush_lsn);
    goto repeat;
  }
}

/** Write to the log file up to the last log entry.
@param durable  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool durable)
{
  ut_ad(!srv_read_only_mode);
  log_write_up_to(log_sys.get_lsn(std::memory_order_acquire), durable);
}

/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.latch. */
ATTRIBUTE_COLD void log_write_and_flush_prepare()
{
  if (log_sys.is_pmem())
    return;

  while (flush_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
  while (write_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
}

/** Durably write the log up to log_sys.get_lsn(). */
ATTRIBUTE_COLD void log_write_and_flush()
{
  ut_ad(!srv_read_only_mode);
  if (!log_sys.is_pmem())
  {
    const lsn_t lsn{log_sys.write_buf<false>()};
    write_lock.release(lsn);
    log_flush(lsn);
  }
#ifdef HAVE_PMEM
  else
    log_sys.persist(log_sys.get_lsn());
#endif
}

/********************************************************************

Tries to establish a big enough margin of free space in the log buffer, such
that a new log entry can be catenated without an immediate need for a flush. */
ATTRIBUTE_COLD static void log_flush_margin()
{
  if (log_sys.buf_free > log_sys.max_buf_free)
    log_buffer_flush_to_disk(false);
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
ATTRIBUTE_COLD static void log_checkpoint_margin()
{
  while (log_sys.check_flush_or_checkpoint())
  {
    log_sys.latch.rd_lock(SRW_LOCK_CALL);
    ut_ad(!recv_no_log_write);

    if (!log_sys.check_flush_or_checkpoint())
    {
func_exit:
      log_sys.latch.rd_unlock();
      return;
    }

    const lsn_t lsn= log_sys.get_lsn();
    const lsn_t checkpoint= log_sys.last_checkpoint_lsn;
    const lsn_t sync_lsn= checkpoint + log_sys.max_checkpoint_age;

    if (lsn <= sync_lsn)
    {
      log_sys.set_check_flush_or_checkpoint(false);
      goto func_exit;
    }

    log_sys.latch.rd_unlock();

    /* We must wait to prevent the tail of the log overwriting the head. */
    buf_flush_wait_flushed(std::min(sync_lsn, checkpoint + (1U << 20)));
    /* Sleep to avoid a thundering herd */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
ATTRIBUTE_COLD void log_check_margins()
{
  do
  {
    log_flush_margin();
    log_checkpoint_margin();
    ut_ad(!recv_no_log_write);
  }
  while (log_sys.check_flush_or_checkpoint());
}

extern void buf_resize_shutdown();

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown()
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */
	bool do_srv_shutdown = false;
	if (srv_master_timer) {
		do_srv_shutdown = srv_fast_shutdown < 2;
		srv_master_timer.reset();
	}

	/* Wait for the end of the buffer resize task.*/
	buf_resize_shutdown();
	dict_stats_shutdown();
	btr_defragment_shutdown();

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

	if (srv_buffer_pool_dump_at_shutdown &&
		!srv_read_only_mode && srv_fast_shutdown < 2) {
		buf_dump_start();
	}
	srv_monitor_timer.reset();

	if (do_srv_shutdown) {
		srv_shutdown(srv_fast_shutdown == 0);
	}


loop:
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(fil_system.is_initialised() || !srv_was_started);

#define COUNT_INTERVAL 600U
#define CHECK_INTERVAL 100000U
	std::this_thread::sleep_for(std::chrono::microseconds(CHECK_INTERVAL));

	count++;

	/* Check that there are no longer transactions, except for
	PREPARED ones. We need this wait even for the 'very fast'
	shutdown, because the InnoDB layer may have committed or
	prepared transactions and we don't want to lose them. */

	if (ulint total_trx = srv_was_started && !srv_read_only_mode
	    && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    ? trx_sys.any_active_transactions() : 0) {

		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			service_manager_extend_timeout(
				COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
				"Waiting for %lu active transactions to finish",
				(ulong) total_trx);
			ib::info() << "Waiting for " << total_trx << " active"
				<< " transactions to finish";

			count = 0;
		}

		goto loop;
	}

	/* We need these threads to stop early in shutdown. */
	const char* thread_name;

   if (srv_fast_shutdown != 2 && trx_rollback_is_active) {
		thread_name = "rollback of recovered transactions";
	} else {
		thread_name = NULL;
	}

	if (thread_name) {
		ut_ad(!srv_read_only_mode);
wait_suspend_loop:
		service_manager_extend_timeout(
			COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
			"Waiting for %s to exit", thread_name);
		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			ib::info() << "Waiting for " << thread_name
				   << " to exit";
			count = 0;
		}
		goto loop;
	}

	/* Check that the background threads are suspended */

	ut_ad(!srv_any_background_activity());
	if (srv_n_fil_crypt_threads_started) {
		fil_crypt_threads_signal(true);
		thread_name = "fil_crypt_thread";
		goto wait_suspend_loop;
	}

	if (buf_page_cleaner_is_active) {
		thread_name = "page cleaner thread";
		pthread_cond_signal(&buf_pool.do_flush_list);
		goto wait_suspend_loop;
	}

	buf_load_dump_end();

	if (!buf_pool.is_initialised()) {
		ut_ad(!srv_was_started);
	} else if (ulint pending_io = buf_pool.io_pending()) {
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for " << pending_io << " buffer"
				" page I/Os to complete";
			count = 0;
		}

		goto loop;
	} else {
		buf_flush_buffer_pool();
	}

	if (srv_fast_shutdown == 2 || !srv_was_started) {
		if (!srv_read_only_mode && srv_was_started) {
			ib::info() << "Executing innodb_fast_shutdown=2."
				" Next startup will execute crash recovery!";

			/* In this fastest shutdown we do not flush the
			buffer pool:

			it is essentially a 'crash' of the InnoDB server.
			Make sure that the log is all flushed to disk, so
			that we can recover all committed transactions in
			a crash recovery. */
			log_buffer_flush_to_disk();
		}

		srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;
		return;
	}

	if (!srv_read_only_mode) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"ensuring dirty buffer pool are written to log");
		log_make_checkpoint();

                const auto sizeof_cp = log_sys.is_encrypted()
			? SIZE_OF_FILE_CHECKPOINT + 8
			: SIZE_OF_FILE_CHECKPOINT;

		log_sys.latch.rd_lock(SRW_LOCK_CALL);

		lsn = log_sys.get_lsn();

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn
			&& lsn != log_sys.last_checkpoint_lsn + sizeof_cp;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		log_sys.latch.rd_unlock();

		if (lsn_changed) {
			goto loop;
		}
	} else {
		lsn = recv_sys.lsn;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
				       "Free innodb buffer pool");
	ut_d(buf_pool.assert_all_freed());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (UNIV_UNLIKELY(lsn < recv_sys.lsn)) {
		sql_print_error("InnoDB: Shutdown LSN=" LSN_PF
				" is less than start LSN=" LSN_PF,
				lsn, recv_sys.lsn);
	}

	srv_shutdown_lsn = lsn;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	log_sys.latch.rd_lock(SRW_LOCK_CALL);

	const lsn_t lsn= log_sys.get_lsn();
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	const lsn_t pages_flushed = buf_pool.get_oldest_modification(lsn);
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		lsn,
		log_sys.get_flushed_lsn(),
		pages_flushed,
		lsn_t{log_sys.last_checkpoint_lsn});

	log_sys.latch.rd_unlock();
}

/** Shut down the redo log subsystem. */
void log_t::close()
{
  ut_ad(this == &log_sys);
  if (!is_initialised()) return;
  close_file();

#ifndef HAVE_PMEM
  ut_free_dodump(buf, buf_size);
  buf= nullptr;
  ut_free_dodump(flush_buf, buf_size);
  flush_buf= nullptr;
  aligned_free(checkpoint_buf);
  checkpoint_buf= nullptr;
#else
  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
#endif

  latch.destroy();
  destroy_lsn_lock();

  recv_sys.close();

  max_buf_free= 0;
}

std::string get_log_file_path(const char *filename)
{
  const size_t size= strlen(srv_log_group_home_dir) + /* path separator */ 1 +
                     strlen(filename) + /* longest suffix */ 3;
  std::string path;
  path.reserve(size);
  path.assign(srv_log_group_home_dir);

  switch (path.back()) {
#ifdef _WIN32
  case '\\':
#endif
  case '/':
    break;
  default:
    path.push_back('/');
  }
  path.append(filename);

  return path;
}
