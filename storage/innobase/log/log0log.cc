/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2023, MariaDB Corporation.

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

#include "arch0arch.h"
#include "log0log.h"
#include "log0crypt.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0stats_bg.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "buf0dump.h"
#include "log0sync.h"
#include "log.h"
#include "tpool.h"

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

void log_t::set_capacity() noexcept
{
	ut_ad(log_sys.latch_have_wr());
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

void log_t::create() noexcept
{
  ut_ad(this == &log_sys);
  ut_ad(!is_initialised());

  latch.SRW_LOCK_INIT(log_latch_key);
  write_lsn_offset= 0;
  /* LSN 0 and 1 are reserved; @see buf_page_t::oldest_modification_ */
  base_lsn.store(FIRST_LSN, std::memory_order_relaxed);
  flushed_to_disk_lsn.store(FIRST_LSN, std::memory_order_relaxed);
  need_checkpoint.store(true, std::memory_order_relaxed);
  write_lsn= FIRST_LSN;

  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
  ut_ad(!writer);

#ifdef HAVE_PMEM
  resize_wrap_mutex.init();
#endif

  last_checkpoint_lsn= FIRST_LSN;
  last_checkpoint_end_lsn= FIRST_LSN;
  log_capacity= 0;
  max_modified_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_lsn= 0;
  checkpoint_pending= false;

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

__attribute__((warn_unused_result))
dberr_t log_file_t::read(os_offset_t offset, span<byte> buf) noexcept
{
  ut_ad(is_opened());
  byte *data= buf.data();
  size_t size= buf.size();
  ut_ad(size);
  ssize_t s;

  for (;;)
  {
    s= IF_WIN(tpool::pread(m_file, data, size, offset),
              pread(m_file, data, size, offset));
    if (UNIV_UNLIKELY(s <= 0))
      break;
    size-= size_t(s);
    if (!size)
      return DB_SUCCESS;
    offset+= s;
    data+= s;
    ut_a(size < buf.size());
  }

  sql_print_error("InnoDB: pread(\"ib_logfile0\") returned %zd,"
                  " operating system error %u",
                  s, unsigned(IF_WIN(GetLastError(), errno)));
  return DB_IO_ERROR;
}

void log_file_t::write(os_offset_t offset, span<const byte> buf) noexcept
{
  ut_ad(is_opened());
  const byte *data= buf.data();
  size_t size= buf.size();
  ut_ad(size);
  ssize_t s;

  for (;;)
  {
    s= IF_WIN(tpool::pwrite(m_file, data, size, offset),
              pwrite(m_file, data, size, offset));
    if (UNIV_UNLIKELY(s <= 0))
      break;
    size-= size_t(s);
    if (!size)
      return;
    offset+= s;
    data+= s;
    ut_a(size < buf.size());
  }

  sql_print_error("[FATAL] InnoDB: pwrite(\"ib_logfile0\") returned %zd,"
                  " operating system error %u",
                  s, unsigned(IF_WIN(GetLastError(), errno)));
  abort();
}

# ifdef HAVE_PMEM
#  include "cache.h"
# endif

/** Attempt to memory map a file.
@param file  log file handle
@param size  file size
@return pointer to memory mapping
@retval MAP_FAILED  if the memory cannot be mapped */
static void *log_mmap(os_file_t file,
# ifdef HAVE_PMEM
                      bool &is_pmem, /*!< whether the file is on pmem */
# endif
                      os_offset_t size)
{
#if SIZEOF_SIZE_T < 8
  if (size != os_offset_t(size_t(size)))
    return MAP_FAILED;
#endif
  if (my_system_page_size > 4096)
    return MAP_FAILED;
# ifndef HAVE_PMEM
  if (!log_sys.log_mmap)
    /* If support for persistent memory (Linux: mount -o dax) is enabled,
    we always attempt to open a MAP_SYNC memory mapping to ib_logfile0.
    This mapping will be read-only during crash recovery, and read-write
    during normal operation.

    A regular read-only memory mapping may be attempted if
    innodb_log_file_mmap=ON. This may benefit mariadb-backup
    and crash recovery. */
    return MAP_FAILED;
# endif

  /* For now, InnoDB does not support memory-mapped writes to
  a regular log file.

  If PMEM is supported, the initially attempted memory mapping may
  be read-write, but the fallback will be read-only.

  The mapping will always be read-only if innodb_read_only=ON or
  if mariadb-backup is running in any other mode than --prepare --export. */
  const bool read_only=
    srv_read_only_mode || srv_operation >= SRV_OPERATION_BACKUP;

# ifdef _WIN32
  void *ptr= MAP_FAILED;
  if (!read_only);
  else if (HANDLE h=
           CreateFileMappingA(file, nullptr, PAGE_READONLY,
                              DWORD(size >> 32), DWORD(size), nullptr))
  {
    if (h != INVALID_HANDLE_VALUE)
    {
      ptr= MapViewOfFileEx(h, FILE_MAP_READ, 0, 0, size, nullptr);
      CloseHandle(h);
      if (!ptr)
        ptr= MAP_FAILED;
    }
  }
# else
  int flags=
#  ifdef HAVE_PMEM
    MAP_SHARED_VALIDATE | MAP_SYNC,
#  else
    MAP_SHARED,
#  endif
    prot= PROT_READ;

  if (!read_only)
#  ifdef HAVE_PMEM
    prot= PROT_READ | PROT_WRITE;

#   ifdef __linux__ /* On Linux, we pretend that /dev/shm is PMEM */
remap:
#   endif
#  else
    return MAP_FAILED;
#  endif

  void *ptr= my_mmap(0, size_t(size), prot, flags, file, 0);

#  ifdef HAVE_PMEM
  is_pmem= ptr != MAP_FAILED;
#  endif

  if (ptr != MAP_FAILED)
    return ptr;

#  ifdef HAVE_PMEM
#   ifdef __linux__ /* On Linux, we pretend that /dev/shm is PMEM */
  if (flags != MAP_SHARED && srv_operation < SRV_OPERATION_BACKUP)
  {
    flags= MAP_SHARED;
    struct stat st;
    if (!fstat(file, &st))
    {
      const auto st_dev= st.st_dev;
      if (!stat("/dev/shm", &st))
      {
        is_pmem= st.st_dev == st_dev;
        if (!is_pmem)
          return ptr; /* MAP_FAILED */
        goto remap;
      }
    }
  }
#   endif /* __linux__ */
  if (read_only && log_sys.log_mmap)
    ptr= my_mmap(0, size_t(size), PROT_READ, MAP_SHARED, file, 0);
#  endif /* HAVE_PMEM */
# endif
  return ptr;
}

#if defined __linux__ || defined _WIN32
/** Display a message about opening the log */
ATTRIBUTE_COLD static void log_file_message() noexcept
{
  sql_print_information("InnoDB: %s (block size=%u bytes)",
                        log_sys.log_mmap
                        ? (log_sys.log_buffered
                           ? "Memory-mapped log"
                           : "Memory-mapped unbuffered log")
                        :
                        log_sys.log_buffered
                        ? "Buffered log writes"
                        : "File system buffers for log disabled",
                        log_sys.write_size);
}
#else
static inline void log_file_message() noexcept {}
#endif

bool log_t::attach(log_file_t file, os_offset_t size) noexcept
{
  log= file;
  ut_ad(!size || size >= START_OFFSET + SIZE_OF_FILE_CHECKPOINT);
  file_size= size;

  ut_ad(!buf);
  ut_ad(!flush_buf);
  ut_ad(!writer);
  if (size)
  {
# ifdef HAVE_PMEM
    bool is_pmem;
    void *ptr= ::log_mmap(log.m_file, is_pmem, size);
# else
    void *ptr= ::log_mmap(log.m_file, size);
# endif
    if (ptr != MAP_FAILED)
    {
# ifdef HAVE_PMEM
      if (is_pmem)
      {
        log.close();
        log_buffered= false;
        log_maybe_unbuffered= true;
        IF_WIN(,mprotect(ptr, size_t(size), PROT_READ));
      }
# endif
      buf= static_cast<byte*>(ptr);
      writer_update(false);
# ifdef HAVE_PMEM
      if (is_pmem)
        return true;
# endif
      goto func_exit;
    }
  }
  log_mmap= false;
  buf= static_cast<byte*>(ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME));
  if (!buf)
  {
  alloc_fail:
    base_lsn.store(0, std::memory_order_relaxed);
    sql_print_error("InnoDB: Cannot allocate memory;"
                    " too large innodb_log_buffer_size?");
    return false;
  }
  flush_buf= static_cast<byte*>(ut_malloc_dontdump(buf_size,
                                                   PSI_INSTRUMENT_ME));
  if (!flush_buf)
  {
  alloc_fail2:
    ut_free_dodump(buf, buf_size);
    buf= nullptr;
    goto alloc_fail;
  }

  ut_ad(ut_is_2pow(write_size));
  ut_ad(write_size >= 512);
  ut_ad(write_size <= 4096);
  checkpoint_buf= static_cast<byte*>(aligned_malloc(write_size, write_size));
  if (!checkpoint_buf)
  {
    ut_free_dodump(flush_buf, buf_size);
    flush_buf= nullptr;
    goto alloc_fail2;
  }

  TRASH_ALLOC(buf, buf_size);
  TRASH_ALLOC(flush_buf, buf_size);
  writer_update(false);
  memset_aligned<512>(checkpoint_buf, 0, write_size);

 func_exit:
  log_file_message();
  return true;
}

/** Write a log file header.
@param buf        log header buffer
@param lsn        log sequence number corresponding to log_sys.START_OFFSET
@param encrypted  whether the log is encrypted */
void log_t::header_write(byte *buf, lsn_t lsn, bool encrypted,
                         bool is_clone) noexcept
{
  mach_write_to_4(my_assume_aligned<4>(buf) + LOG_HEADER_FORMAT,
                  log_sys.FORMAT_10_8);
  mach_write_to_8(my_assume_aligned<8>(buf + LOG_HEADER_START_LSN), lsn);

#if defined __GNUC__ && __GNUC__ > 7
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
  std::string clone_header(log_t::CREATOR_CLONE);
  clone_header.append(PACKAGE_VERSION);
  strncpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
          is_clone ? clone_header.c_str() : "MariaDB " PACKAGE_VERSION,
          LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR);
#if defined __GNUC__ && __GNUC__ > 7
# pragma GCC diagnostic pop
#endif

  if (encrypted)
    log_crypt_write_header(buf + LOG_HEADER_CREATOR_END);
  mach_write_to_4(my_assume_aligned<4>(508 + buf), my_crc32c(0, buf, 508));
}

void log_t::create(lsn_t lsn) noexcept
{
  ut_ad(latch_have_wr());
  ut_ad(!recv_no_log_write);
  ut_ad(is_latest());
  ut_ad(this == &log_sys);

  write_lsn_offset= 0;
  base_lsn.store(lsn, std::memory_order_relaxed);
  flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
  first_lsn= lsn;
  write_lsn= lsn;

  last_checkpoint_lsn= 0;
  last_checkpoint_end_lsn= 0;

  DBUG_PRINT("ib_log", ("write header " LSN_PF, lsn));

#ifdef HAVE_PMEM
  if (is_mmap())
  {
    ut_ad(!is_opened());
    mprotect(buf, size_t(file_size), PROT_READ | PROT_WRITE);
    memset_aligned<4096>(buf, 0, 4096);
    log_sys.header_write(buf, lsn, is_encrypted());
    pmem_persist(buf, 512);
    buf_size= unsigned(std::min<uint64_t>(capacity(), buf_size_max));
  }
  else
#endif
  {
    ut_ad(!is_mmap());
    memset_aligned<4096>(flush_buf, 0, buf_size);
    memset_aligned<4096>(buf, 0, buf_size);
    log_sys.header_write(buf, lsn, is_encrypted());
    log.write(0, {buf, 4096});
    memset_aligned<512>(buf, 0, 512);
  }
}

ATTRIBUTE_COLD static void log_close_failed(dberr_t err) noexcept
{
  ib::fatal() << "closing ib_logfile0 failed: " << err;
}

void log_t::close_file(bool really_close) noexcept
{
  if (is_mmap())
  {
    ut_ad(!checkpoint_buf);
    ut_ad(!flush_buf);
    if (buf)
    {
      my_munmap(buf, size_t(file_size));
      buf= nullptr;
    }
  }
  else
  {
    ut_ad(!buf == !flush_buf);
    ut_ad(!buf == !checkpoint_buf);
    if (buf)
    {
      ut_free_dodump(buf, buf_size);
      buf= nullptr;
      ut_free_dodump(flush_buf, buf_size);
      flush_buf= nullptr;
    }
    aligned_free(checkpoint_buf);
    checkpoint_buf= nullptr;
  }

  writer= nullptr;

  if (really_close)
    if (is_opened())
      if (const dberr_t err= log.close())
        log_close_failed(err);
}

/** @return the current log sequence number (may be stale) */
lsn_t log_get_lsn() noexcept
{
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  lsn_t lsn= log_sys.get_lsn();
  log_sys.latch.wr_unlock();
  return lsn;
}

/** Acquire all latches that protect the log. */
static void log_resize_acquire() noexcept
{
#ifdef HAVE_PMEM
  if (!log_sys.is_mmap())
#endif
  {
    while (flush_lock.acquire(log_get_lsn() + 1, nullptr) !=
           group_commit_lock::ACQUIRED);
    while (write_lock.acquire(log_get_lsn() + 1, nullptr) !=
           group_commit_lock::ACQUIRED);
  }

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
}

/** Release the latches that protect the log. */
void log_resize_release() noexcept
{
  log_sys.latch.wr_unlock();

#ifdef HAVE_PMEM
  if (!log_sys.is_mmap())
#endif
  {
    lsn_t lsn1= write_lock.release(write_lock.value());
    lsn_t lsn2= flush_lock.release(flush_lock.value());
    if (lsn1 || lsn2)
      log_write_up_to(std::max(lsn1, lsn2), true, nullptr);
  }
}

#if defined __linux__ || defined _WIN32
/** Try to enable or disable file system caching (update log_buffered) */
void log_t::set_buffered(bool buffered) noexcept
{
  if (!log_maybe_unbuffered ||
#ifdef HAVE_PMEM
      is_mmap() ||
#endif
      high_level_read_only)
    return;
  log_resize_acquire();
  if (!resize_in_progress() && is_opened() && bool(log_buffered) != buffered)
  {
    if (const dberr_t err= log.close())
      log_close_failed(err);
    std::string path{get_log_file_path()};
    log_buffered= buffered;
    bool success;
    log.m_file= os_file_create_func(path.c_str(),
                                    OS_FILE_OPEN, OS_LOG_FILE,
                                    false, &success);
    ut_a(log.m_file != OS_FILE_CLOSED);
    log_file_message();
  }
  log_resize_release();
}
#endif

  /** Try to enable or disable durable writes (update log_write_through) */
void log_t::set_write_through(bool write_through)
{
  if (is_mmap() || high_level_read_only)
    return;
  log_resize_acquire();
  if (!resize_in_progress() && is_opened() &&
      bool(log_write_through) != write_through)
  {
    os_file_close_func(log.m_file);
    log.m_file= OS_FILE_CLOSED;
    std::string path{get_log_file_path()};
    log_write_through= write_through;
    bool success;
    log.m_file= os_file_create_func(path.c_str(),
                                    OS_FILE_OPEN, OS_LOG_FILE,
                                    false, &success);
    ut_a(log.m_file != OS_FILE_CLOSED);
    sql_print_information(log_write_through
                          ? "InnoDB: Log writes write through"
                          : "InnoDB: Log writes may be cached");
  }
  log_resize_release();
}

/** Start resizing the log and release the exclusive latch.
@param size  requested new file_size
@param thd   the current thread identifier
@return whether the resizing was started successfully */
log_t::resize_start_status log_t::resize_start(os_offset_t size, void *thd)
  noexcept
{
  ut_ad(size >= 4U << 20);
  ut_ad(!(size & 4095));
  ut_ad(!srv_read_only_mode);
  ut_ad(thd);

  log_resize_acquire();

  resize_start_status status;

  if (size == file_size)
    status= RESIZE_NO_CHANGE;
  else if (resize_in_progress())
    status= RESIZE_IN_PROGRESS;
  else
  {
    lsn_t start_lsn;
    ut_ad(!resize_in_progress());
    ut_ad(!resize_log.is_opened());
    ut_ad(!resize_buf);
    ut_ad(!resize_flush_buf);
    ut_ad(!resize_initiator);
    std::string path{get_log_file_path("ib_logfile101")};
    bool success;
    resize_initiator= thd;
    resize_lsn.store(1, std::memory_order_relaxed);
    resize_target= 0;
    resize_log.m_file=
      os_file_create_func(path.c_str(), OS_FILE_CREATE,
                          OS_LOG_FILE, false, &success);
    if (success)
    {
      ut_ad(!(size_t(file_size) & (write_size - 1)));
      ut_ad(!(size_t(size) & (write_size - 1)));
      log_resize_release();

      void *ptr= nullptr, *ptr2= nullptr;
      success= os_file_set_size(path.c_str(), resize_log.m_file, size);
      if (!success);
#ifdef HAVE_PMEM
      else if (is_mmap())
      {
        bool is_pmem{false};
        ptr= ::log_mmap(resize_log.m_file, is_pmem, size);

        if (ptr == MAP_FAILED)
          goto alloc_fail;
      }
#endif
      else
      {
        ut_ad(!is_mmap());
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
        start_lsn= get_lsn();

        if (!is_mmap())
          start_lsn= first_lsn +
            (~lsn_t{write_size - 1} &
             (lsn_t{write_size - 1} + start_lsn - first_lsn));
        else if (!is_opened())
          resize_log.close();

        resize_lsn.store(start_lsn, std::memory_order_relaxed);
        writer_update(true);
        log_resize_release();

        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        lsn_t target_lsn= buf_pool.get_oldest_modification(0);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        buf_flush_ahead(start_lsn < target_lsn ? target_lsn + 1 : start_lsn,
                        false);
        return RESIZE_STARTED;
      }
    }
    resize_initiator= nullptr;
    resize_lsn.store(0, std::memory_order_relaxed);
    status= RESIZE_FAILED;
  }

  log_resize_release();
  return status;
}

/** Abort a resize_start() that we started. */
void log_t::resize_abort(void *thd) noexcept
{
  log_resize_acquire();

  if (resize_running(thd))
  {
#ifdef HAVE_PMEM
    const bool is_mmap{this->is_mmap()};
#else
    constexpr bool is_mmap{false};
#endif
    if (!is_mmap)
    {
      ut_free_dodump(resize_buf, buf_size);
      ut_free_dodump(resize_flush_buf, buf_size);
      resize_flush_buf= nullptr;
    }
    else
    {
      ut_ad(!resize_log.is_opened());
      ut_ad(!resize_flush_buf);
#ifdef HAVE_PMEM
      if (resize_buf)
        my_munmap(resize_buf, resize_target);
#endif /* HAVE_PMEM */
    }
    if (resize_log.is_opened())
      resize_log.close();
    resize_buf= nullptr;
    resize_target= 0;
    resize_lsn.store(0, std::memory_order_relaxed);
    resize_initiator= nullptr;
    std::string path{get_log_file_path("ib_logfile101")};
    IF_WIN(DeleteFile(path.c_str()), unlink(path.c_str()));
    writer_update(false);
  }

  log_resize_release();
}

/** Write an aligned buffer to ib_logfile0.
@param buf    buffer to be written
@param length length of data to be written
@param offset log file offset */
static void log_write_buf(const byte *buf, size_t length, lsn_t offset)
{
  ut_ad(write_lock.is_owner());
  ut_ad(!recv_no_log_write);
  ut_d(const size_t block_size_1= log_sys.write_size - 1);
  ut_ad(!(offset & block_size_1));
  ut_ad(!(length & block_size_1));
  ut_ad(!(size_t(buf) & block_size_1));
  ut_ad(length);

  const lsn_t maximum_write_length{log_sys.file_size - offset};
  ut_ad(maximum_write_length <= log_sys.file_size - log_sys.START_OFFSET);

  if (UNIV_UNLIKELY(length > maximum_write_length))
  {
    log_sys.log.write(offset, {buf, size_t(maximum_write_length)});
    length-= size_t(maximum_write_length);
    buf+= size_t(maximum_write_length);
    ut_ad(log_sys.START_OFFSET + length < offset);
    offset= log_sys.START_OFFSET;
  }
  log_sys.log.write(offset, {buf, length});
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
void log_t::persist(lsn_t lsn) noexcept
{
  ut_ad(!is_opened());
  ut_ad(!write_lock.is_owner());
  ut_ad(!flush_lock.is_owner());
  ut_ad(latch_have_wr());

  lsn_t old= flushed_to_disk_lsn.load(std::memory_order_relaxed);

  if (old >= lsn)
    return;

  const size_t start(calc_lsn_offset(old));
  const size_t end(calc_lsn_offset(lsn));

  if (UNIV_UNLIKELY(end < start))
  {
    pmem_persist(buf + start, file_size - start);
    pmem_persist(buf + START_OFFSET, end - START_OFFSET);
  }
  else
    pmem_persist(buf + start, end - start);

  uint64_t offset{write_lsn_offset};
  const lsn_t new_base_lsn= base_lsn.load(std::memory_order_relaxed) +
    (offset & (WRITE_BACKOFF - 1));
  ut_ad(new_base_lsn >= lsn);
  write_to_buf+= size_t(offset >> WRITE_TO_BUF_SHIFT);
  /* This synchronizes with get_lsn_approx();
  we must store write_lsn_offset before base_lsn. */
  write_lsn_offset.store(0, std::memory_order_relaxed);
  base_lsn.store(new_base_lsn, std::memory_order_release);
  flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
  log_flush_notify(lsn);
  DBUG_EXECUTE_IF("crash_after_log_write_upto", DBUG_SUICIDE(););
}

ATTRIBUTE_NOINLINE
static void log_write_persist(lsn_t lsn) noexcept
{
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  log_sys.persist(lsn);
  log_sys.latch.wr_unlock();
}
#endif

void log_t::resize_write_buf(const byte *b, size_t length) noexcept
{
  const size_t block_size_1= write_size - 1;
  ut_ad(b == resize_buf || b == resize_flush_buf);
  ut_ad(!(resize_target & block_size_1));
  ut_ad(!(length & block_size_1));
  ut_ad(length > block_size_1);
  ut_ad(length <= resize_target);

  int64_t d= int64_t(write_lsn - resize_in_progress());
  if (UNIV_UNLIKELY(d < 0))
  {
    d&= ~int64_t(block_size_1);
    if (int64_t(d + length) <= 0)
      return;
    length+= ssize_t(d);
    b-= ssize_t(d);
    d= 0;
  }
  lsn_t offset= START_OFFSET + (lsn_t(d) & ~lsn_t{block_size_1}) %
    (resize_target - START_OFFSET);

  if (UNIV_UNLIKELY(offset + length > resize_target))
  {
    offset= START_OFFSET;
    resize_lsn.store(first_lsn +
                     (~lsn_t{block_size_1} & (write_lsn - first_lsn)),
                     std::memory_order_relaxed);
  }

  ut_a(os_file_write_func(IORequestWrite, "ib_logfile101", resize_log.m_file,
                          b, offset, length) == DB_SUCCESS);
}

/** Write buf to ib_logfile0 and possibly ib_logfile101.
@tparam resizing whether to release latch and whether resize_in_progress()>1
@return the current log sequence number */
template<log_t::resizing_and_latch resizing>
inline __attribute__((always_inline))
lsn_t log_t::write_buf() noexcept
{
  ut_ad(latch_have_wr());
  ut_ad(!is_mmap());
  ut_ad(!srv_read_only_mode);
  ut_ad(resizing == RETAIN_LATCH ||
        (resizing == RESIZING) == (resize_in_progress() > 1));

  const lsn_t lsn{get_lsn()};

  if (write_lsn >= lsn)
  {
    if (resizing != RETAIN_LATCH)
      latch.wr_unlock();
    ut_ad(write_lsn == lsn);
  }
  else
  {
    ut_ad(write_lock.is_owner());
    ut_ad(!recv_no_log_write);
    write_lock.set_pending(lsn);
    ut_ad(write_lsn >= get_flushed_lsn());
    const size_t write_size_1{write_size - 1};
    ut_ad(ut_is_2pow(write_size));
    lsn_t base= base_lsn.load(std::memory_order_relaxed);
    size_t length{size_t(lsn - base)};
    lsn_t offset{calc_lsn_offset(write_lsn)};
    ut_ad(length >= (offset & write_size_1));
    ut_ad(write_size_1 >= 511);

    const byte *const write_buf{buf};
    byte *const re_write_buf{resizing == NOT_RESIZING ? nullptr : resize_buf};
    ut_ad(resizing == RETAIN_LATCH ||
          (resizing == NOT_RESIZING) == !re_write_buf);
    ut_ad(!re_write_buf == !resize_flush_buf);
    if (resizing == RESIZING)
#ifdef _MSC_VER
      __assume(re_write_buf != nullptr);
#else
      if (!re_write_buf) __builtin_unreachable();
#endif
    offset&= ~lsn_t{write_size_1};

    if (length <= write_size_1)
    {
      ut_ad(!((length ^ (size_t(lsn) - size_t(first_lsn))) & write_size_1));
      /* Keep filling the same buffer until we have more than one block. */
      MEM_MAKE_DEFINED(buf + length, (write_size_1 + 1) - length);
      buf[length]= 0; /* ensure that recovery catches EOF */
      if (UNIV_LIKELY_NULL(re_write_buf))
      {
        MEM_MAKE_DEFINED(re_write_buf + length, (write_size_1 + 1) - length);
        re_write_buf[length]= 0;
      }
      length= write_size_1 + 1;
    }
    else
    {
      const size_t new_buf_free{length & write_size_1};
      base+= length & ~write_size_1;
      ut_ad(new_buf_free == ((lsn - first_lsn) & write_size_1));
      write_to_buf+= size_t(write_lsn_offset >> WRITE_TO_BUF_SHIFT);
      /* This synchronizes with get_lsn_approx();
      we must store write_lsn_offset before base_lsn. */
      write_lsn_offset.store(new_buf_free, std::memory_order_relaxed);
      base_lsn.store(base, std::memory_order_release);

      if (new_buf_free)
      {
        /* The rest of the block will be written as garbage.
        (We want to avoid memset() while holding exclusive log_sys.latch)
        This block will be overwritten later, once records beyond
        the current LSN are generated. */
        MEM_MAKE_DEFINED(buf + length, (write_size_1 + 1) - new_buf_free);
        buf[length]= 0; /* allow recovery to catch EOF faster */
        if (UNIV_LIKELY_NULL(re_write_buf))
          MEM_MAKE_DEFINED(re_write_buf + length, (write_size_1 + 1) -
                           new_buf_free);
        length&= ~write_size_1;
        memcpy_aligned<16>(flush_buf, buf + length, (new_buf_free + 15) & ~15);
        if (UNIV_LIKELY_NULL(re_write_buf))
        {
          memcpy_aligned<16>(resize_flush_buf, re_write_buf + length,
                             (new_buf_free + 15) & ~15);
          re_write_buf[length + new_buf_free]= 0;
        }
        length+= write_size_1 + 1;
      }

      std::swap(buf, flush_buf);
      if (UNIV_LIKELY_NULL(re_write_buf))
        std::swap(resize_buf, resize_flush_buf);
    }

    ut_ad(base + (write_lsn_offset & (WRITE_TO_BUF - 1)) == lsn);
    write_to_log++;
    arch_sys->log_sys()->wait_archiver(lsn);

    if (resizing != RETAIN_LATCH)
      latch.wr_unlock();

    DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF " at " LSN_PF,
                          write_lsn, lsn, offset));

    /* Do the write to the log file */
    log_write_buf(write_buf, length, offset);

    if (UNIV_LIKELY_NULL(re_write_buf))
      resize_write_buf(re_write_buf, length);
    write_lsn= lsn;

    if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED))
    {
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "InnoDB log write: " LSN_PF, write_lsn);
    }
  }

  set_check_for_checkpoint(false);
  return lsn;
}

bool log_t::flush(lsn_t lsn) noexcept
{
  ut_ad(lsn >= get_flushed_lsn());
  flush_lock.set_pending(lsn);
  const bool success{log_write_through || log.flush()};
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
static lsn_t log_flush(lsn_t lsn) noexcept
{
  ut_ad(!log_sys.is_mmap());
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
                     const completion_callback *callback) noexcept
{
  ut_ad(!srv_read_only_mode);
  ut_ad(lsn != LSN_MAX);
  ut_ad(lsn != 0);
#ifdef HAVE_PMEM
  if (log_sys.is_mmap())
  {
    if (durable)
      log_write_persist(lsn);
    else
      ut_ad(!callback);
    return;
  }
#endif
  ut_ad(!log_sys.is_mmap());

repeat:
  if (durable)
  {
    if (flush_lock.acquire(lsn, callback) != group_commit_lock::ACQUIRED)
      return;
    /* Promise to other concurrent flush_lock.acquire() that we
    will be durable at least up to the current LSN. The LSN may still
    advance when we acquire log_sys.latch below. */
    if (lsn > log_sys.get_flushed_lsn())
      flush_lock.set_pending(lsn);
  }

  lsn_t pending_write_lsn= 0, pending_flush_lsn= 0;

  if (write_lock.acquire(lsn, durable ? nullptr : callback) ==
      group_commit_lock::ACQUIRED)
  {
    ut_ad(!recv_no_log_write || srv_operation != SRV_OPERATION_NORMAL);
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    pending_write_lsn= write_lock.release(log_sys.writer());
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

static lsn_t log_writer() noexcept
{
  return log_sys.write_buf<log_t::NOT_RESIZING>();
}

ATTRIBUTE_COLD static lsn_t log_writer_resizing() noexcept
{
  return log_sys.write_buf<log_t::RESIZING>();
}

void log_t::writer_update(bool resizing) noexcept
{
  ut_ad(latch_have_wr());
  ut_ad(resizing == (resize_in_progress() > 1));
  writer= resizing ? log_writer_resizing : log_writer;
  mtr_t::finisher_update();
}

/** Write to the log file up to the last log entry.
@param durable  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool durable) noexcept
{
  log_write_up_to(log_get_lsn(), durable);
}

void log_t::get_last_block(lsn_t &last_lsn, byte *last_block,
                           uint32_t block_len)
{
  ut_ad(ut_is_2pow(block_len));
  ut_ad(block_len <= write_size);

  latch.wr_lock(SRW_LOCK_CALL);
  last_lsn= get_lsn(std::memory_order_relaxed);

  auto block_len_1= static_cast<size_t>(block_len - 1);

  size_t data_len{buf_free.load(std::memory_order_relaxed)};
  size_t offset= data_len & ~block_len_1;

  data_len&= block_len_1;
  std::memcpy(last_block, buf + offset, data_len);

  latch.wr_unlock();
  std::memset(last_block + data_len, 0x00, (size_t)block_len - data_len);
}

/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.latch. */
ATTRIBUTE_COLD void log_write_and_flush_prepare() noexcept
{
#ifdef HAVE_PMEM
  if (log_sys.is_mmap())
    return;
#endif

  while (flush_lock.acquire(log_get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
  while (write_lock.acquire(log_get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
}

void log_t::clear_mmap() noexcept
{
  if (!is_mmap() || high_level_read_only)
    return;
#ifdef HAVE_PMEM
  if (!is_opened())
  {
    ut_d(latch.wr_lock(SRW_LOCK_CALL));
    ut_ad(!resize_in_progress());
    ut_ad(get_lsn() == get_flushed_lsn(std::memory_order_relaxed));
    ut_d(latch.wr_unlock());
    return;
  }
#endif

  log_resize_acquire();
  ut_ad(!resize_in_progress());
  ut_ad(write_lsn == get_lsn());
  ut_ad(write_lsn == get_flushed_lsn(std::memory_order_relaxed));

  if (buf) /* this may be invoked while creating a new database */
  {
    alignas(16) byte log_block[4096];
    const size_t bs{write_size};
    {
      const size_t bf=
        size_t(write_lsn - base_lsn.load(std::memory_order_relaxed));
      memcpy_aligned<16>(log_block, buf + (bf & ~(bs - 1)), bs);
    }

    close_file(false);
    log_mmap= false;
    ut_a(attach(log, file_size));
    ut_ad(!is_mmap());

    memcpy_aligned<16>(buf, log_block, bs);
  }
  log_resize_release();
}

/** Durably write the log up to log_sys.get_lsn(). */
ATTRIBUTE_COLD void log_write_and_flush() noexcept
{
  ut_ad(!srv_read_only_mode);
#ifdef HAVE_PMEM
  if (log_sys.is_mmap())
    log_sys.persist(log_sys.get_lsn());
  else
#endif
  {
    const lsn_t lsn{log_sys.write_buf<log_t::RETAIN_LATCH>()};
    write_lock.release(lsn);
    log_flush(lsn);
  }
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
ATTRIBUTE_COLD static void log_checkpoint_margin() noexcept
{
  while (log_sys.check_for_checkpoint())
  {
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    ut_ad(!recv_no_log_write);

    if (!log_sys.check_for_checkpoint())
    {
func_exit:
      log_sys.latch.wr_unlock();
      return;
    }

    const lsn_t lsn= log_sys.get_lsn();
    const lsn_t checkpoint= log_sys.last_checkpoint_lsn;
    const lsn_t sync_lsn= checkpoint + log_sys.max_checkpoint_age;

    if (lsn <= sync_lsn)
    {
#ifndef DBUG_OFF
    skip_checkpoint:
#endif
      log_sys.set_check_for_checkpoint(false);
      goto func_exit;
    }

    DBUG_EXECUTE_IF("ib_log_checkpoint_avoid_hard", goto skip_checkpoint;);
    log_sys.latch.wr_unlock();

    /* We must wait to prevent the tail of the log overwriting the head. */
    buf_flush_wait_flushed(std::min(sync_lsn, checkpoint + (1U << 20)));
    /* Sleep to avoid a thundering herd */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/** Wait for a log checkpoint if needed.
NOTE that this function may only be called while not holding
any synchronization objects except dict_sys.latch. */
void log_free_check() noexcept
{
  ut_ad(!lock_sys.is_holder());
  if (log_sys.check_for_checkpoint())
  {
    ut_ad(!recv_no_log_write);
    log_checkpoint_margin();
  }
}

#ifdef __linux__
extern void buf_mem_pressure_shutdown() noexcept;
#else
inline void buf_mem_pressure_shutdown() noexcept {}
#endif

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown() noexcept
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master task and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */
	if (srv_master_timer) {
		srv_master_timer.reset();
	}

	buf_mem_pressure_shutdown();
	dict_stats_shutdown();

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

	if (srv_buffer_pool_dump_at_shutdown &&
		!srv_read_only_mode && srv_fast_shutdown < 2) {
		buf_dump_start();
	}
	srv_monitor_timer.reset();

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
	const char* thread_name= nullptr;

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
	rollback_all_recovered_task.wait();

	if (!buf_pool.is_initialised()) {
		ut_ad(!srv_was_started);
	} else {
		buf_flush_buffer_pool();
	}

	if (srv_fast_shutdown == 2 || !srv_was_started) {
		if (!srv_read_only_mode && srv_was_started) {
			sql_print_information(
				"InnoDB: Executing innodb_fast_shutdown=2."
				" Next startup will execute crash recovery!");

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

		log_sys.latch.wr_lock(SRW_LOCK_CALL);

		lsn = log_sys.get_lsn();

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn
			&& lsn != log_sys.last_checkpoint_lsn + sizeof_cp;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		log_sys.latch.wr_unlock();

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

	ut_a(lsn == log_get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (UNIV_UNLIKELY(lsn < recv_sys.lsn)) {
		sql_print_error("InnoDB: Shutdown LSN=" LSN_PF
				" is less than start LSN=" LSN_PF,
				lsn, recv_sys.lsn);
	}

	srv_shutdown_lsn = lsn;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	ut_a(lsn == log_get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	log_sys.latch.wr_lock(SRW_LOCK_CALL);

	const lsn_t lsn= log_sys.get_lsn();
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	const lsn_t pages_flushed = buf_pool.get_oldest_modification(lsn);
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
	const lsn_t flushed_lsn{log_sys.get_flushed_lsn()};
	const lsn_t checkpoint_lsn{log_sys.last_checkpoint_lsn};
	log_sys.latch.wr_unlock();

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		lsn, flushed_lsn, pages_flushed, checkpoint_lsn);
}

/** Shut down the redo log subsystem. */
void log_t::close()
{
  ut_ad(this == &log_sys);
  if (!is_initialised()) return;
  close_file();

  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
  base_lsn.store(0, std::memory_order_relaxed);

  latch.destroy();
#ifdef HAVE_PMEM
  resize_wrap_mutex.destroy();
#endif

  recv_sys.close();
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
