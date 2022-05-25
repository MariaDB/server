/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/log0log.h
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "log0types.h"
#include "os0file.h"
#include "span.h"
#include "my_atomic_wrapper.h"
#include "srw_lock.h"
#include <string>

using st_::span;

static const char LOG_FILE_NAME_PREFIX[] = "ib_logfile";
static const char LOG_FILE_NAME[] = "ib_logfile0";

/** Composes full path for a redo log file
@param[in]	filename	name of the redo log file
@return path with log file name*/
std::string get_log_file_path(const char *filename= LOG_FILE_NAME);

/** Delete log file.
@param[in]	suffix	suffix of the file name */
static inline void delete_log_file(const char* suffix)
{
  auto path = get_log_file_path(LOG_FILE_NAME_PREFIX).append(suffix);
  os_file_delete_if_exists_func(path.c_str(), nullptr);
}

struct completion_callback;

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param lsn      log sequence number that should be included in the file write
@param durable  whether the write needs to be durable
@param callback log write completion callback */
void log_write_up_to(lsn_t lsn, bool durable,
                     const completion_callback *callback= nullptr);

/** Write to the log file up to the last log entry.
@param durable  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool durable= true);


/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.latch. */
ATTRIBUTE_COLD void log_write_and_flush_prepare();

/** Durably write the log up to log_sys.get_lsn(). */
ATTRIBUTE_COLD void log_write_and_flush();

/** Make a checkpoint */
ATTRIBUTE_COLD void log_make_checkpoint();

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown();

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
ATTRIBUTE_COLD void log_check_margins();

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file);	/*!< in: file where to print */

/** Offsets of a log file header */
/* @{ */
/** Log file header format identifier (32-bit unsigned big-endian integer).
This used to be called LOG_GROUP_ID and always written as 0,
because InnoDB never supported more than one copy of the redo log. */
#define LOG_HEADER_FORMAT	0
/** LSN of the start of data in this log file (with format version 1;
in format version 0, it was called LOG_FILE_START_LSN and at offset 4). */
#define LOG_HEADER_START_LSN	8
/** A null-terminated string which will contain either the string 'ibbackup'
and the creation time if the log file was created by mysqlbackup --restore,
or the MySQL version that created the redo log file. */
#define LOG_HEADER_CREATOR	16
/** End of the log file creator field. */
#define LOG_HEADER_CREATOR_END	48
/* @} */

struct log_t;

/** File abstraction */
class log_file_t
{
  friend log_t;
  os_file_t m_file{OS_FILE_CLOSED};
public:
  log_file_t()= default;
  log_file_t(os_file_t file) noexcept : m_file(file) {}

  /** Open a file
  @return file size in bytes
  @retval 0 if not readable */
  os_offset_t open(bool read_only) noexcept;
  bool is_opened() const noexcept { return m_file != OS_FILE_CLOSED; }

  dberr_t close() noexcept;
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept;
  void write(os_offset_t offset, span<const byte> buf) noexcept;
  bool flush() const noexcept { return os_file_flush(m_file); }
#ifdef HAVE_PMEM
  byte *mmap(bool read_only, const struct stat &st) noexcept;
#endif
};

/** Redo log buffer */
struct log_t
{
  /** The original (not version-tagged) InnoDB redo log format */
  static constexpr uint32_t FORMAT_3_23= 0;
  /** The MySQL 5.7.9/MariaDB 10.2.2 log format */
  static constexpr uint32_t FORMAT_10_2= 1;
  /** The MariaDB 10.3.2 log format. */
  static constexpr uint32_t FORMAT_10_3= 103;
  /** The MariaDB 10.4.0 log format. */
  static constexpr uint32_t FORMAT_10_4= 104;
  /** Encrypted MariaDB redo log */
  static constexpr uint32_t FORMAT_ENCRYPTED= 1U << 31;
  /** The MariaDB 10.4.0 log format (only with innodb_encrypt_log=ON) */
  static constexpr uint32_t FORMAT_ENC_10_4= FORMAT_10_4 | FORMAT_ENCRYPTED;
  /** The MariaDB 10.5.1 physical redo log format */
  static constexpr uint32_t FORMAT_10_5= 0x50485953;
  /** The MariaDB 10.5.1 physical format (only with innodb_encrypt_log=ON) */
  static constexpr uint32_t FORMAT_ENC_10_5= FORMAT_10_5 | FORMAT_ENCRYPTED;
  /** The MariaDB 10.8.0 variable-block-size redo log format */
  static constexpr uint32_t FORMAT_10_8= 0x50687973;
  /** The MariaDB 10.8.0 format with innodb_encrypt_log=ON */
  static constexpr uint32_t FORMAT_ENC_10_8= FORMAT_10_8 | FORMAT_ENCRYPTED;

  /** Location of the first checkpoint block */
  static constexpr size_t CHECKPOINT_1= 4096;
  /** Location of the second checkpoint block */
  static constexpr size_t CHECKPOINT_2= 8192;
  /** Start of record payload */
  static constexpr lsn_t START_OFFSET= 12288;

  /** smallest possible log sequence number in the current format
  (used to be 2048 before FORMAT_10_8). */
  static constexpr lsn_t FIRST_LSN= START_OFFSET;

private:
  /** The log sequence number of the last change of durable InnoDB files */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  std::atomic<lsn_t> lsn;
  /** the first guaranteed-durable log sequence number */
  std::atomic<lsn_t> flushed_to_disk_lsn;
  /** log sequence number when log resizing was initiated, or 0 */
  std::atomic<lsn_t> resize_lsn;
  /** set when there may be need to flush the log buffer, or
  preflush buffer pool pages, or initiate a log checkpoint.
  This must hold if lsn - last_checkpoint_lsn > max_checkpoint_age. */
  std::atomic<bool> check_flush_or_checkpoint_;


#if defined(__aarch64__)
/* On ARM, we do more spinning */
typedef srw_spin_lock log_rwlock_t;
#define LSN_LOCK_ATTR MY_MUTEX_INIT_FAST
#else
typedef srw_lock log_rwlock_t;
#define LSN_LOCK_ATTR nullptr
#endif

public:
  /** rw-lock protecting buf */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) log_rwlock_t latch;
private:
  /** Last written LSN */
  lsn_t write_lsn;
public:
  /** log record buffer, written to by mtr_t::commit() */
  byte *buf;
  /** buffer for writing data to ib_logfile0, or nullptr if is_pmem()
  In write_buf(), buf and flush_buf are swapped */
  byte *flush_buf;
  /** number of std::swap(buf, flush_buf) and writes from buf to log;
  protected by latch.wr_lock() */
  ulint write_to_log;
  /** innodb_log_buffer_size (size of buf,flush_buf if !is_pmem(), in bytes) */
  size_t buf_size;

private:
  /** Log file being constructed during resizing; protected by latch */
  log_file_t resize_log;
  /** size of resize_log; protected by latch */
  lsn_t resize_target;
  /** Buffer for writing to resize_log; @see buf */
  byte *resize_buf;
  /** Buffer for writing to resize_log; @see flush_buf */
  byte *resize_flush_buf;

  /** spin lock protecting lsn, buf_free in append_prepare() */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) pthread_mutex_t lsn_lock;
  void init_lsn_lock() { pthread_mutex_init(&lsn_lock, LSN_LOCK_ATTR); }
  void lock_lsn() { pthread_mutex_lock(&lsn_lock); }
  void unlock_lsn() { pthread_mutex_unlock(&lsn_lock); }
  void destroy_lsn_lock() { pthread_mutex_destroy(&lsn_lock); }

public:
  /** first free offset within buf use; protected by lsn_lock */
  Atomic_relaxed<size_t> buf_free;
  /** number of write requests (to buf); protected by exclusive lsn_lock */
  ulint write_to_buf;
  /** number of waits in append_prepare(); protected by lsn_lock */
  ulint waits;
  /** recommended maximum size of buf, after which the buffer is flushed */
  size_t max_buf_free;

  /** log file size in bytes, including the header */
  lsn_t file_size;
private:
  /** the log sequence number at the start of the log file */
  lsn_t first_lsn;
#if defined __linux__ || defined _WIN32
  /** The physical block size of the storage */
  uint32_t block_size;
#endif
public:
  /** format of the redo log: e.g., FORMAT_10_8 */
  uint32_t format;
  /** Log file */
  log_file_t log;

	/** Fields involved in checkpoints @{ */
	lsn_t		log_capacity;	/*!< capacity of the log; if
					the checkpoint age exceeds this, it is
					a serious error because it is possible
					we will then overwrite log and spoil
					crash recovery */
	lsn_t		max_modified_age_async;
					/*!< when this recommended
					value for lsn -
					buf_pool.get_oldest_modification()
					is exceeded, we start an
					asynchronous preflush of pool pages */
	lsn_t		max_checkpoint_age;
					/*!< this is the maximum allowed value
					for lsn - last_checkpoint_lsn when a
					new query step is started */
  /** latest completed checkpoint (protected by latch.wr_lock()) */
  Atomic_relaxed<lsn_t> last_checkpoint_lsn;
  /** next checkpoint LSN (protected by log_sys.mutex) */
  lsn_t next_checkpoint_lsn;
  /** next checkpoint number (protected by latch.wr_lock()) */
  ulint next_checkpoint_no;
  /** whether a checkpoint is pending */
  Atomic_relaxed<bool> checkpoint_pending;

  /** buffer for checkpoint header */
  byte *checkpoint_buf;
	/* @} */

  bool is_initialised() const noexcept { return max_buf_free != 0; }

#ifdef HAVE_PMEM
  bool is_pmem() const noexcept { return !flush_buf; }
#else
  static constexpr bool is_pmem() { return false; }
#endif

  bool is_opened() const noexcept { return log.is_opened(); }

  /** @return LSN at which log resizing was started and is still in progress
      @retval 0 if no log resizing is in progress */
  lsn_t resize_in_progress() const noexcept
  { return resize_lsn.load(std::memory_order_relaxed); }

  /** Status of resize_start() */
  enum resize_start_status {
    RESIZE_NO_CHANGE, RESIZE_IN_PROGRESS, RESIZE_STARTED, RESIZE_FAILED
  };

  /** Start resizing the log and release the exclusive latch.
  @param size  requested new file_size
  @return whether the resizing was started successfully */
  resize_start_status resize_start(os_offset_t size) noexcept;

  /** Abort any resize_start(). */
  void resize_abort() noexcept;

  /** Replicate a write to the log.
  @param lsn  start LSN
  @param end  end of the mini-transaction
  @param len  length of the mini-transaction
  @param seq  offset of the sequence bit from the end */
  inline void resize_write(lsn_t lsn, const byte *end,
                           size_t len, size_t seq) noexcept;

  /** Write resize_buf to resize_log.
  @param length  the used length of resize_buf */
  ATTRIBUTE_COLD void resize_write_buf(size_t length) noexcept;

  /** Rename a log file after resizing.
  @return whether an error occurred */
  static bool resize_rename() noexcept;

#ifdef HAVE_PMEM
  /** @return pointer for writing to resize_buf
  @retval nullptr if no PMEM based resizing is active */
  inline byte *resize_buf_begin(lsn_t lsn) const noexcept;
  /** @return end of resize_buf */
  inline const byte *resize_buf_end() const noexcept
  { return resize_buf + resize_target; }
#endif

  void attach(log_file_t file, os_offset_t size);

  void close_file();

  /** Calculate the checkpoint safety margins. */
  static void set_capacity();

  /** Write a log file header.
  @param buf        log header buffer
  @param lsn        log sequence number corresponding to log_sys.START_OFFSET
  @param encrypted  whether the log is encrypted */
  static void header_write(byte *buf, lsn_t lsn, bool encrypted);

  lsn_t get_lsn(std::memory_order order= std::memory_order_relaxed) const
  { return lsn.load(order); }
  void set_lsn(lsn_t lsn) { this->lsn.store(lsn, std::memory_order_release); }

  lsn_t get_flushed_lsn(std::memory_order order= std::memory_order_acquire)
    const noexcept
  { return flushed_to_disk_lsn.load(order); }

  /** Initialize the LSN on initial log file creation. */
  lsn_t init_lsn() noexcept
  {
    latch.wr_lock(SRW_LOCK_CALL);
    const lsn_t lsn{get_lsn()};
    flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
    write_lsn= lsn;
    latch.wr_unlock();
    return lsn;
  }

  void set_recovered_lsn(lsn_t lsn) noexcept
  {
#ifndef SUX_LOCK_GENERIC
    ut_ad(latch.is_write_locked());
#endif /* SUX_LOCK_GENERIC */
    write_lsn= lsn;
    this->lsn.store(lsn, std::memory_order_relaxed);
    flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
  }

#ifdef HAVE_PMEM
  /** Persist the log.
  @param lsn    desired new value of flushed_to_disk_lsn */
  inline void persist(lsn_t lsn) noexcept;
#endif

  bool check_flush_or_checkpoint() const
  {
    return UNIV_UNLIKELY
      (check_flush_or_checkpoint_.load(std::memory_order_relaxed));
  }
  void set_check_flush_or_checkpoint(bool flag= true)
  { check_flush_or_checkpoint_.store(flag, std::memory_order_relaxed); }

  /** Make previous write_buf() durable and update flushed_to_disk_lsn. */
  bool flush(lsn_t lsn) noexcept;

  /** Initialise the redo log subsystem. */
  void create();

  /** Shut down the redo log subsystem. */
  void close();

#if defined __linux__ || defined _WIN32
  /** @return the physical block size of the storage */
  size_t get_block_size() const noexcept
  { ut_ad(block_size); return block_size; }
  /** Set the log block size for file I/O. */
  void set_block_size(uint32_t size) noexcept { block_size= size; }
#else
  /** @return the physical block size of the storage */
  static size_t get_block_size() { return 512; }
#endif

private:
  /** Wait in append_prepare() for buffer to become available
  @param ex   whether log_sys.latch is exclusively locked */
  ATTRIBUTE_COLD static void append_prepare_wait(bool ex) noexcept;
public:
  /** Reserve space in the log buffer for appending data.
  @tparam pmem  log_sys.is_pmem()
  @param size   total length of the data to append(), in bytes
  @param ex     whether log_sys.latch is exclusively locked
  @return the start LSN and the buffer position for append() */
  template<bool pmem>
  inline std::pair<lsn_t,byte*> append_prepare(size_t size, bool ex) noexcept;

  /** Append a string of bytes to the redo log.
  @param d     destination
  @param s     string of bytes
  @param size  length of str, in bytes */
  void append(byte *&d, const void *s, size_t size) noexcept
  {
#ifndef SUX_LOCK_GENERIC
    ut_ad(latch.is_locked());
#endif
    ut_ad(d + size <= buf + (is_pmem() ? file_size : buf_size));
    memcpy(d, s, size);
    d+= size;
  }

  /** Set the log file format. */
  void set_latest_format(bool encrypted) noexcept
  { format= encrypted ? FORMAT_ENC_10_8 : FORMAT_10_8; }
  /** @return whether the redo log is encrypted */
  bool is_encrypted() const noexcept { return format & FORMAT_ENCRYPTED; }
  /** @return whether the redo log is in the latest format */
  bool is_latest() const noexcept
  { return (~FORMAT_ENCRYPTED & format) == FORMAT_10_8; }

  /** @return capacity in bytes */
  lsn_t capacity() const noexcept { return file_size - START_OFFSET; }

  /** Set the LSN of the log file at file creation. */
  void set_first_lsn(lsn_t lsn) noexcept { write_lsn= first_lsn= lsn; }
  /** @return the first LSN of the log file */
  lsn_t get_first_lsn() const noexcept { return first_lsn; }

  /** Determine the sequence bit at a log sequence number */
  byte get_sequence_bit(lsn_t lsn) const noexcept
  {
    ut_ad(lsn >= first_lsn);
    return !(((lsn - first_lsn) / capacity()) & 1);
  }

  /** Calculate the offset of a log sequence number.
      @param lsn   log sequence number
      @return byte offset within ib_logfile0 */
  lsn_t calc_lsn_offset(lsn_t lsn) const noexcept
  {
    ut_ad(lsn >= first_lsn);
    return START_OFFSET + (lsn - first_lsn) % capacity();
  }

  /** Write checkpoint information and invoke latch.wr_unlock().
  @param end_lsn    start LSN of the FILE_CHECKPOINT mini-transaction */
  inline void write_checkpoint(lsn_t end_lsn) noexcept;

  /** Write buf to ib_logfile0.
  @tparam release_latch whether to invoke latch.wr_unlock()
  @return the current log sequence number */
  template<bool release_latch> inline lsn_t write_buf() noexcept;

  /** Create the log. */
  void create(lsn_t lsn) noexcept;
};

/** Redo log system */
extern log_t	log_sys;

inline void log_free_check()
{
  if (log_sys.check_flush_or_checkpoint())
    log_check_margins();
}

/** Release the latches that protect log resizing. */
void log_resize_release();
