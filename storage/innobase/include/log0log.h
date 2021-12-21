/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
#include <vector>
#include <string>

using st_::span;

extern ulong srv_log_buffer_size;

static const char LOG_FILE_NAME_PREFIX[] = "ib_logfile";
static const char LOG_FILE_NAME[] = "ib_logfile0";

/** Composes full path for a redo log file
@param[in]	filename	name of the redo log file
@return path with log file name*/
std::string get_log_file_path(const char *filename= LOG_FILE_NAME);

/** Returns paths for all existing log files */
std::vector<std::string> get_existing_log_files_paths();

/** Delete log file.
@param[in]	suffix	suffix of the file name */
static inline void delete_log_file(const char* suffix)
{
  auto path = get_log_file_path(LOG_FILE_NAME_PREFIX).append(suffix);
  os_file_delete_if_exists(innodb_log_file_key, path.c_str(), nullptr);
}

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */
void log_buffer_extend(ulong len);

/** Calculate the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_pool.get_oldest_modification().
@param[in]	file_size	requested innodb_log_file_size
@retval true on success
@retval false if the smallest log is too small to
accommodate the number of OS threads in the database server */
bool
log_set_capacity(ulonglong file_size)
	MY_ATTRIBUTE((warn_unused_result));

struct completion_callback;

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param lsn      log sequence number that should be included in the file write
@param durable  whether the log should be durably written
@param callback log write completion callback */
void log_write_up_to(lsn_t lsn, bool durable,
                     const completion_callback *callback= nullptr);

/** Write to the log file up to the last log entry.
@param sync  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool sync= true);


/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.mutex. */
ATTRIBUTE_COLD void log_write_and_flush_prepare();

/** Durably write the log up to log_sys.lsn() and release log_sys.mutex. */
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
/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
void
log_refresh_stats(void);
/*===================*/

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

/** Abstraction for reading, writing and flushing file cache to disk */
class file_io
{
public:
  file_io(bool durable_writes= false) : m_durable_writes(durable_writes) {}
  virtual ~file_io() noexcept {};
  virtual dberr_t open(const char *path, bool read_only) noexcept= 0;
  virtual dberr_t rename(const char *old_path,
                         const char *new_path) noexcept= 0;
  virtual dberr_t close() noexcept= 0;
  virtual dberr_t read(os_offset_t offset, span<byte> buf) noexcept= 0;
  virtual dberr_t write(const char *path, os_offset_t offset,
                        span<const byte> buf) noexcept= 0;
  virtual dberr_t flush() noexcept= 0;

  /** Durable writes doesn't require calling flush() */
  bool writes_are_durable() const noexcept { return m_durable_writes; }

protected:
  bool m_durable_writes;
};

class file_os_io final: public file_io
{
public:
  file_os_io()= default;
  file_os_io(const file_os_io &)= delete;
  file_os_io &operator=(const file_os_io &)= delete;
  file_os_io(file_os_io &&rhs);
  file_os_io &operator=(file_os_io &&rhs);
  ~file_os_io() noexcept;

  dberr_t open(const char *path, bool read_only) noexcept final;
  bool is_opened() const noexcept { return m_fd != OS_FILE_CLOSED; }
  dberr_t rename(const char *old_path, const char *new_path) noexcept final;
  dberr_t close() noexcept final;
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept final;
  dberr_t write(const char *path, os_offset_t offset,
                span<const byte> buf) noexcept final;
  dberr_t flush() noexcept final;

private:
  pfs_os_file_t m_fd{OS_FILE_CLOSED};
};

/** File abstraction + path */
class log_file_t
{
public:
  log_file_t(std::string path= "") noexcept : m_path{std::move(path)} {}

  dberr_t open(bool read_only) noexcept;
  bool is_opened() const noexcept;

  const std::string &get_path() const noexcept { return m_path; }

  dberr_t rename(std::string new_path) noexcept;
  dberr_t close() noexcept;
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept;
  bool writes_are_durable() const noexcept;
  dberr_t write(os_offset_t offset, span<const byte> buf) noexcept;
  dberr_t flush() noexcept;
  void free()
  {
    m_path.clear();
    m_path.shrink_to_fit();
  }

private:
  std::unique_ptr<file_io> m_file;
  std::string m_path;
};

/** Redo log buffer */
struct log_t{
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

  /** The physical block size of the storage; FIXME: determine from OS */
  static constexpr size_t BLOCK_SIZE{512};

private:
  /** The log sequence number of the last change of durable InnoDB files */
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE)
  std::atomic<lsn_t> lsn;
  /** the first guaranteed-durable log sequence number */
  std::atomic<lsn_t> flushed_to_disk_lsn;
  /** set when there may be need to flush the log buffer, or
  preflush buffer pool pages, or initiate a log checkpoint.
  This must hold if lsn - last_checkpoint_lsn > max_checkpoint_age. */
  std::atomic<bool> check_flush_or_checkpoint_;
public:
  /** mutex protecting the log */
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t mutex;
  /** first free offset within the log buffer in use */
  size_t buf_free;
  /** recommended maximum size of buf, after which the buffer is flushed */
  size_t max_buf_free;
  /** mutex to serialize access to the flush list when we are putting
  dirty blocks in the list. The idea behind this mutex is to be able
  to release log_sys.mutex during mtr_commit and still ensure that
  insertions in the flush_list happen in the LSN order. */
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t flush_order_mutex;
  /** log_buffer, append data here */
  byte *buf;
  /** log_buffer, writing data to file from this buffer.
  Before flushing write_buf is swapped with flush_buf */
  byte *flush_buf;

  /** log file size in bytes, including the header */
  lsn_t file_size;
private:
  /** the log sequence number at the start of the log file */
  lsn_t first_lsn;
public:
  /** format of the redo log: e.g., FORMAT_10_8 */
  uint32_t format;
  /** Log file */
  class file {
    /** log file */
    log_file_t fd;
  public:
    /** opens log file which must be closed prior this call */
    void open_file(std::string path);
    /** writes header */
    void write_header_durable(lsn_t lsn);
    /** opens log file which must be closed prior this call */
    dberr_t rename(std::string path) { return fd.rename(path); }
    /** reads buffer from log file
    @param[in]	offset		offset in log file
    @param[in]	buf		buffer where to read */
    void read(os_offset_t offset, span<byte> buf);
    /** Tells whether writes require calling flush() */
    bool writes_are_durable() const noexcept;
    /** writes buffer to log file
    @param[in]	offset		offset in log file
    @param[in]	buf		buffer from which to write */
    void write(os_offset_t offset, span<const byte> buf);
    /** flushes OS page cache (excluding metadata!) for log file */
    void flush();
    /** closes log file */
    void close_file();

    /** Close the redo log buffer. */
    void close() { close_file(); }
  } log;

	/** The fields involved in the log buffer flush @{ */

	lsn_t		write_lsn;	/*!< last written lsn */
	lsn_t		current_flush_lsn;/*!< end lsn for the current running
					write + flush operation */
	std::atomic<size_t> pending_flushes; /*!< system calls in progress */
	std::atomic<size_t> flushes;	/*!< system calls counter */

	ulint		n_log_ios;	/*!< number of log i/os initiated thus
					far */
	ulint		n_log_ios_old;	/*!< number of log i/o's at the
					previous printout */
	time_t		last_printout_time;/*!< when log_print was last time
					called */
	/* @} */

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
  /** latest completed checkpoint (protected by log_sys.mutex) */
  Atomic_relaxed<lsn_t> last_checkpoint_lsn;
	lsn_t		next_checkpoint_lsn;
					/*!< next checkpoint lsn */
  /** next checkpoint number (protected by mutex) */
  ulint next_checkpoint_no;
  /** number of pending checkpoint writes */
  ulint n_pending_checkpoint_writes;

  /** buffer for checkpoint header */
  byte *checkpoint_buf;
	/* @} */

  bool is_initialised() const noexcept { return max_buf_free != 0; }

  lsn_t get_lsn(std::memory_order order= std::memory_order_relaxed) const
  { return lsn.load(order); }
  void set_lsn(lsn_t lsn) { this->lsn.store(lsn, std::memory_order_release); }

  lsn_t get_flushed_lsn() const
  { return flushed_to_disk_lsn.load(std::memory_order_acquire); }
  void set_flushed_lsn(lsn_t lsn)
  { flushed_to_disk_lsn.store(lsn, std::memory_order_release); }

  bool check_flush_or_checkpoint() const
  {
    return UNIV_UNLIKELY
      (check_flush_or_checkpoint_.load(std::memory_order_relaxed));
  }
  void set_check_flush_or_checkpoint(bool flag= true)
  { check_flush_or_checkpoint_.store(flag, std::memory_order_relaxed); }

  size_t get_pending_flushes() const
  {
    return pending_flushes.load(std::memory_order_relaxed);
  }

  size_t get_flushes() const
  {
    return flushes.load(std::memory_order_relaxed);
  }

  /** Initialise the redo log subsystem. */
  void create();

  /** Shut down the redo log subsystem. */
  void close();

  /** Reserve space in the log buffer for appending data.
  @param size   upper limit of the length of the data to append(), in bytes
  @return the current LSN */
  inline lsn_t append_prepare(size_t size) noexcept;

  /** Append a string of bytes to the redo log.
  @param s     string of bytes
  @param size  length of str, in bytes */
  void append(const void *s, size_t size) noexcept
  {
    mysql_mutex_assert_owner(&mutex);
    memcpy(buf + buf_free, s, size);
    buf_free+= size;
    ut_ad(buf_free <= size_t{srv_log_buffer_size});
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
  lsn_t capacity() const{ return file_size - START_OFFSET; }

  /** Set the LSN of the log file at file creation. */
  void set_first_lsn(lsn_t lsn) noexcept { first_lsn= lsn; }
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

  /** Write checkpoint information to the log header and release mutex.
  @param end_lsn    start LSN of the FILE_CHECKPOINT mini-transaction */
  inline void write_checkpoint(lsn_t end_lsn) noexcept;

  /** Write the log buffer to the file and release the mutex.
  @param lsn  the current log sequence number */
  void write(lsn_t lsn) noexcept;

  /** Create the log. */
  inline void create(lsn_t lsn) noexcept;
};

/** Redo log system */
extern log_t	log_sys;
#ifdef UNIV_DEBUG
extern bool log_write_lock_own();
#endif

inline void log_free_check()
{
  if (log_sys.check_flush_or_checkpoint())
    log_check_margins();
}
