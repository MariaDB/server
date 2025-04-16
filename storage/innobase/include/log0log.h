/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
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
                     const completion_callback *callback= nullptr) noexcept;

/** Write to the log file up to the last log entry.
@param durable  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool durable= true) noexcept;

/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.latch. */
ATTRIBUTE_COLD void log_write_and_flush_prepare() noexcept;

/** Durably write the log up to log_sys.get_lsn(). */
ATTRIBUTE_COLD void log_write_and_flush() noexcept;

/** Make a checkpoint */
ATTRIBUTE_COLD void log_make_checkpoint() noexcept;

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown() noexcept;

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

  /** @return whether a handle to the log is open */
  bool is_opened() const noexcept { return m_file != OS_FILE_CLOSED; }

  dberr_t close() noexcept;
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept;
  void write(os_offset_t offset, span<const byte> buf) noexcept;
  bool flush() const noexcept { return os_file_flush(m_file); }
};

/** Redo log buffer */
struct log_t
{
  /** The maximum buf_size */
  static constexpr unsigned buf_size_max= os_file_request_size_max;

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
  /** the least significant bit of the write_to_buf buffer */
  static constexpr size_t WRITE_TO_BUF_SHIFT{34};
  /** write_lsn_offset component for incrementing write_to_buf */
  static constexpr uint64_t WRITE_TO_BUF{1ULL << WRITE_TO_BUF_SHIFT};
  /** write_lsn_offset flag to indicate that append_prepare_wait() is active */
  static constexpr uint64_t WRITE_BACKOFF{1ULL << 33};

  /** The current log sequence number, relative to base_lsn, and flags;
  may be modified while latch_have_any() */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  Atomic_relaxed<uint64_t> write_lsn_offset;
  /** the LSN of the last write_buf() or persist(); protected by latch */
  std::atomic<lsn_t> base_lsn;
  /** the first guaranteed-durable log sequence number */
  std::atomic<lsn_t> flushed_to_disk_lsn;
public:
  /** innodb_log_buffer_size (usable append_prepare() size in bytes) */
  unsigned buf_size;
  /** log file size in bytes, including the header */
  lsn_t file_size;

#ifdef LOG_LATCH_DEBUG
  typedef srw_lock_debug log_rwlock;

  bool latch_have_wr() const { return latch.have_wr(); }
  bool latch_have_rd() const { return latch.have_rd(); }
  bool latch_have_any() const { return latch.have_any(); }
#else
  typedef srw_lock log_rwlock;
# ifndef UNIV_DEBUG
# elif defined SUX_LOCK_GENERIC
  bool latch_have_wr() const { return true; }
  bool latch_have_rd() const { return true; }
  bool latch_have_any() const { return true; }
# else
  bool latch_have_wr() const { return latch.is_write_locked(); }
  bool latch_have_rd() const { return latch.is_locked(); }
  bool latch_have_any() const { return latch.is_locked(); }
# endif
#endif
  /** latch_have_wr() for checkpoint, latch_have_any() for append_prepare() */
  log_rwlock latch;

  /** log record buffer, written to by mtr_t::commit() */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) byte *buf;

  /** number of write requests to buf,
  excluding (write_lsn_offset & WRITE_TO_BUF);
  protected by latch.wr_lock() */
  size_t write_to_buf;

  /** number of writes from buf or flush_buf to log;
  protected by latch.wr_lock() */
  size_t write_to_log;

  /** Last written LSN; protected by latch */
  lsn_t write_lsn;

  /** Buffer for writing data to ib_logfile0, or nullptr if is_mmap().
  In write_buf(), buf and flush_buf may be swapped */
  byte *flush_buf;

  /** set when there may be need to initiate a log checkpoint.
  This must hold if lsn - last_checkpoint_lsn > max_checkpoint_age. */
  std::atomic<bool> need_checkpoint;
  /** whether a checkpoint is pending; protected by latch.wr_lock() */
  Atomic_relaxed<bool> checkpoint_pending;
  /** next checkpoint number (protected by latch.wr_lock()) */
  byte next_checkpoint_no;
  /** Log sequence number when a log file overwrite (broken crash recovery)
  was noticed. Protected by latch.wr_lock(). */
  lsn_t overwrite_warned;

  /** latest completed checkpoint (protected by latch.wr_lock()) */
  Atomic_relaxed<lsn_t> last_checkpoint_lsn;
  /** The log writer (protected by latch.wr_lock()) */
  lsn_t (*writer)() noexcept;
  /** next checkpoint LSN (protected by latch.wr_lock()) */
  lsn_t next_checkpoint_lsn;

  /** Log file */
  log_file_t log;
private:
  /** Log file being constructed during resizing; protected by latch */
  log_file_t resize_log;
  /** size of resize_log; protected by latch */
  lsn_t resize_target;
  /** Buffer for writing to resize_log; @see buf */
  byte *resize_buf;
  /** Buffer for writing to resize_log; @see flush_buf */
  byte *resize_flush_buf;

  /** log sequence number when log resizing was initiated;
  0 if the log is not being resized, 1 if resize_start() is in progress */
  std::atomic<lsn_t> resize_lsn;
  /** the log sequence number at the start of the log file */
  lsn_t first_lsn;
public:
  /** current innodb_log_write_ahead_size */
  uint write_size;
  /** format of the redo log: e.g., FORMAT_10_8 */
  uint32_t format;
  /** whether the memory-mapped interface is enabled for the log */
  my_bool log_mmap;
  /** the default value of log_mmap */
  static constexpr bool log_mmap_default=
# if defined __linux__ /* MAP_POPULATE would enable read-ahead */
    true ||
# elif defined __FreeBSD__ /* MAP_PREFAULT_READ would enable read-ahead */
    true ||
# else /* an unnecessary read-ahead of a large ib_logfile0 is a risk */
# endif
    false;
#if defined __linux__ || defined _WIN32
  /** whether file system caching is enabled for the log */
  my_bool log_buffered;
# ifdef _WIN32
  static constexpr bool log_maybe_unbuffered= true;
# else
  /** whether file system caching may be disabled */
  bool log_maybe_unbuffered;
# endif
#endif
  /** whether each write to ib_logfile0 is durable (O_DSYNC) */
  my_bool log_write_through;

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

  /** buffer for checkpoint header */
  byte *checkpoint_buf;
	/* @} */

private:
  /** the thread that initiated resize_lsn() */
  Atomic_relaxed<void*> resize_initiator;
#ifdef HAVE_PMEM
  /** mutex protecting wrap-around in resize_write() */
  srw_mutex resize_wrap_mutex;
#endif
public:
  /** number of long append_prepare_wait(); protected by latch_have_wr() */
  size_t waits;

  bool is_initialised() const noexcept
  { return base_lsn.load(std::memory_order_relaxed) != 0; }
  inline void set_recovered() noexcept;

  bool is_mmap() const noexcept { return !flush_buf; }

  /** @return whether a handle to the log is open;
  is_mmap() && !is_opened() holds for PMEM */
  bool is_opened() const noexcept { return log.is_opened(); }

  /** @return LSN at which log resizing was started and is still in progress
      @retval 0 if no log resizing is in progress
      @retval 1 if resize_start() is in progress */
  lsn_t resize_in_progress() const noexcept
  { return resize_lsn.load(std::memory_order_relaxed); }

  /** Status of resize_start() */
  enum resize_start_status {
    RESIZE_NO_CHANGE, RESIZE_IN_PROGRESS, RESIZE_STARTED, RESIZE_FAILED
  };

  /** Start resizing the log and release the exclusive latch.
  @param size  requested new file_size
  @param thd   the current thread identifier
  @return whether the resizing was started successfully */
  resize_start_status resize_start(os_offset_t size, void *thd) noexcept;

  /** Abort a resize_start() that we started.
  @param thd  thread identifier that had been passed to resize_start() */
  void resize_abort(void *thd) noexcept;

  /** @return whether a particular resize_start() is in progress */
  bool resize_running(void *thd) const noexcept
  { return thd == resize_initiator; }

  /** Replicate a write to the log.
  @param lsn  start LSN
  @param end  end of the mini-transaction
  @param len  length of the mini-transaction
  @param seq  offset of the sequence bit from the end */
  inline void resize_write(lsn_t lsn, const byte *end,
                           size_t len, size_t seq) noexcept;

private:
  /** Write resize_buf to resize_log.
  @param b       resize_buf or resize_flush_buf
  @param length  the used length of b */
  void resize_write_buf(const byte *b, size_t length) noexcept;
public:

  /** Rename a log file after resizing.
  @return whether an error occurred */
  static bool resize_rename() noexcept;

  /** @return pointer for writing to resize_buf
  @retval nullptr if no is_mmap() based resizing is active */
  inline byte *resize_buf_begin(lsn_t lsn) const noexcept;
  /** @return end of resize_buf */
  inline const byte *resize_buf_end() const noexcept
  { return resize_buf + resize_target; }

  /** Initialise the redo log subsystem. */
  void create() noexcept;

  /** Attach a log file.
  @return whether the memory allocation succeeded */
  bool attach(log_file_t file, os_offset_t size) noexcept;

  /** Disable memory-mapped access (update log_mmap) */
  void clear_mmap() noexcept;
  void close_file(bool really_close= true) noexcept;
#if defined __linux__ || defined _WIN32
  /** Try to enable or disable file system caching (update log_buffered) */
  void set_buffered(bool buffered) noexcept;
#endif
  /** Try to enable or disable durable writes (update log_write_through) */
  void set_write_through(bool write_through);

  /** Calculate the checkpoint safety margins. */
  static void set_capacity() noexcept;

  /** Write a log file header.
  @param buf        log header buffer
  @param lsn        log sequence number corresponding to log_sys.START_OFFSET
  @param encrypted  whether the log is encrypted */
  static void header_write(byte *buf, lsn_t lsn, bool encrypted) noexcept;

  /** @return a lower bound estimate of get_lsn(),
  using acquire-release ordering with write_buf() or persist();
  this is exact unless append_prepare_wait() is pending */
  lsn_t get_lsn_approx() const noexcept
  {
    /* acquire-release ordering with write_buf() and persist() */
    lsn_t lsn= base_lsn.load(std::memory_order_acquire);
    lsn += write_lsn_offset.load(std::memory_order_relaxed) &
      (WRITE_BACKOFF - 1);
    return lsn;
  }

  /** @return the current log sequence number (logical time stamp) */
  lsn_t get_lsn() const noexcept
  {
    ut_ad(latch_have_wr());
    return base_lsn.load(std::memory_order_relaxed) +
      (write_lsn_offset & (WRITE_BACKOFF - 1));
  }

  lsn_t get_flushed_lsn(std::memory_order order= std::memory_order_acquire)
    const noexcept
  { return flushed_to_disk_lsn.load(order); }

  /** Initialize the LSN on initial log file creation. */
  inline lsn_t init_lsn() noexcept;

  void set_recovered_lsn(lsn_t lsn) noexcept
  {
    ut_ad(latch_have_wr());
    uint64_t lsn_offset= ((write_size - 1) & (lsn - first_lsn));
    write_lsn_offset= lsn_offset;
    base_lsn.store(lsn - lsn_offset, std::memory_order_relaxed);
    flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
    write_lsn= lsn;
  }

#ifdef HAVE_PMEM
  /** Persist the log.
  @param lsn            desired new value of flushed_to_disk_lsn */
  void persist(lsn_t lsn) noexcept;
#endif

  bool check_for_checkpoint() const
  {
    return UNIV_UNLIKELY(need_checkpoint.load(std::memory_order_relaxed));
  }
  void set_check_for_checkpoint(bool need= true)
  {
    need_checkpoint.store(need, std::memory_order_relaxed);
  }

  /** Make previous write_buf() durable and update flushed_to_disk_lsn. */
  bool flush(lsn_t lsn) noexcept;

  /** Shut down the redo log subsystem. */
  void close();

#if defined __linux__ || defined _WIN32
  /** Set the log block size for file I/O. */
  void set_block_size(uint32 size) noexcept
  {
    if (write_size < size)
      write_size= size;
  }
#endif

private:
  /** Update writer and mtr_t::finisher */
  void writer_update(bool resizing) noexcept;

  /** Wait in append_prepare() for buffer to become available
  @param late   whether the WRITE_BACKOFF flag had already been set
  @param ex     whether log_sys.latch is exclusively locked */
  ATTRIBUTE_COLD void append_prepare_wait(bool late, bool ex) noexcept;
public:
  /** Reserve space in the log buffer for appending data.
  @tparam mmap  log_sys.is_mmap()
  @param size   total length of the data to append(), in bytes
  @param ex     whether log_sys.latch is exclusively locked
  @return the start LSN and the buffer position for append() */
  template<bool mmap>
  std::pair<lsn_t,byte*> append_prepare(size_t size, bool ex) noexcept;

  /** Append a string of bytes to the redo log.
  @param d     destination
  @param s     string of bytes
  @param size  length of str, in bytes */
  static inline void append(byte *&d, const void *s, size_t size) noexcept;

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

  /** Variations of write_buf() */
  enum resizing_and_latch {
    /** skip latch.wr_unlock(); log resizing may or may not be in progress */
    RETAIN_LATCH,
    /** invoke latch.wr_unlock(); !(resize_in_progress() > 1) */
    NOT_RESIZING,
    /** invoke latch.wr_unlock(); resize_in_progress() > 1 */
    RESIZING
  };

  /** Write buf to ib_logfile0 and possibly ib_logfile101.
  @tparam resizing whether to release latch and whether resize_in_progress()>1
  @return the current log sequence number */
  template<resizing_and_latch resizing> inline lsn_t write_buf() noexcept;

  /** Create the log. */
  void create(lsn_t lsn) noexcept;
};

/** Redo log system */
extern log_t	log_sys;

/** Wait for a log checkpoint if needed.
NOTE that this function may only be called while not holding
any synchronization objects except dict_sys.latch. */
void log_free_check() noexcept;

/** @return the current log sequence number (may be stale) */
lsn_t log_get_lsn() noexcept;

/** Release the latches that protect log resizing. */
void log_resize_release() noexcept;
