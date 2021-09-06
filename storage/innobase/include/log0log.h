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

#ifndef log0log_h
#define log0log_h

#include "log0types.h"
#include "os0file.h"
#include "span.h"
#include "my_atomic_wrapper.h"
#include <vector>
#include <string>

using st_::span;

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

/** Append a string to the log.
@param[in]	str		string
@param[in]	len		string length
@param[out]	start_lsn	start LSN of the log record
@return end lsn of the log record, zero if did not succeed */
UNIV_INLINE
lsn_t
log_reserve_and_write_fast(
	const void*	str,
	ulint		len,
	lsn_t*		start_lsn);
/***********************************************************************//**
Checks if there is need for a log buffer flush or a new checkpoint, and does
this if yes. Any database operation should call this when it has modified
more than about 4 pages. NOTE that this function may only be called when the
OS thread owns no synchronization objects except dict_sys.latch. */
UNIV_INLINE
void
log_free_check(void);
/*================*/

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

/**
Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param[in]	lsn		log sequence number that should be
included in the redo log file write
@param[in]	flush_to_disk	whether the written log should also
be flushed to the file system
@param[in]	rotate_key	whether to rotate the encryption key
@param[in]  cb completion callback. If not NULL, the callback will be called
  whenever lsn is written or flushed.
*/
struct completion_callback;
void log_write_up_to(lsn_t lsn, bool flush_to_disk, bool rotate_key = false,
  const completion_callback* cb=nullptr);

/** write to the log file up to the last log entry.
@param[in]	sync	whether we want the written log
also to be flushed to disk. */
void
log_buffer_flush_to_disk(
	bool sync = true);

/** Make a checkpoint */
ATTRIBUTE_COLD void log_make_checkpoint();

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown();

/** Write checkpoint info to the log header and release log_sys.mutex.
@param[in]	end_lsn	start LSN of the FILE_CHECKPOINT mini-transaction */
ATTRIBUTE_COLD void log_write_checkpoint_info(lsn_t end_lsn);

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
ATTRIBUTE_COLD void log_check_margins();

/************************************************************//**
Gets a log block flush bit.
@return TRUE if this block was the first to be written in a log flush */
UNIV_INLINE
ibool
log_block_get_flush_bit(
/*====================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Gets a log block number stored in the header.
@return log block number stored in the block header */
UNIV_INLINE
ulint
log_block_get_hdr_no(
/*=================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Gets a log block data length.
@return log block data length measured as a byte offset from the block start */
UNIV_INLINE
ulint
log_block_get_data_len(
/*===================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets the log block data length. */
UNIV_INLINE
void
log_block_set_data_len(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	len);		/*!< in: data length */
/** Calculate the CRC-32C checksum of a log block.
@param[in]	block	log block
@return checksum */
inline ulint log_block_calc_checksum_crc32(const byte* block);

/************************************************************//**
Gets a log block checksum field value.
@return checksum */
UNIV_INLINE
ulint
log_block_get_checksum(
/*===================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets a log block checksum field value. */
UNIV_INLINE
void
log_block_set_checksum(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	checksum);	/*!< in: checksum */
/************************************************************//**
Gets a log block first mtr log record group offset.
@return first mtr log record group byte offset from the block start, 0
if none */
UNIV_INLINE
ulint
log_block_get_first_rec_group(
/*==========================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets the log block first mtr log record group offset. */
UNIV_INLINE
void
log_block_set_first_rec_group(
/*==========================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	offset);	/*!< in: offset, 0 if none */
/************************************************************//**
Gets a log block checkpoint number field (4 lowest bytes).
@return checkpoint no (4 lowest bytes) */
UNIV_INLINE
ulint
log_block_get_checkpoint_no(
/*========================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Initializes a log block in the log buffer. */
UNIV_INLINE
void
log_block_init(
/*===========*/
	byte*	log_block,	/*!< in: pointer to the log buffer */
	lsn_t	lsn);		/*!< in: lsn within the log block */
/************************************************************//**
Converts a lsn to a log block number.
@return log block number, it is > 0 and <= 1G */
UNIV_INLINE
ulint
log_block_convert_lsn_to_no(
/*========================*/
	lsn_t	lsn);	/*!< in: lsn of a byte within the block */
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

/* The counting of lsn's starts from this value: this must be non-zero */
#define LOG_START_LSN		((lsn_t) (16 * OS_FILE_LOG_BLOCK_SIZE))

/* Offsets of a log block header */
#define	LOG_BLOCK_HDR_NO	0	/* block number which must be > 0 and
					is allowed to wrap around at 2G; the
					highest bit is set to 1 if this is the
					first log block in a log flush write
					segment */
#define LOG_BLOCK_FLUSH_BIT_MASK 0x80000000UL
					/* mask used to get the highest bit in
					the preceding field */
#define	LOG_BLOCK_HDR_DATA_LEN	4	/* number of bytes of log written to
					this block */
#define	LOG_BLOCK_FIRST_REC_GROUP 6	/* offset of the first start of an
					mtr log record group in this log block,
					0 if none; if the value is the same
					as LOG_BLOCK_HDR_DATA_LEN, it means
					that the first rec group has not yet
					been catenated to this log block, but
					if it will, it will start at this
					offset; an archive recovery can
					start parsing the log records starting
					from this offset in this log block,
					if value not 0 */
#define LOG_BLOCK_CHECKPOINT_NO	8	/* 4 lower bytes of the value of
					log_sys.next_checkpoint_no when the
					log block was last written to: if the
					block has not yet been written full,
					this value is only updated before a
					log buffer flush */
#define LOG_BLOCK_HDR_SIZE	12	/* size of the log block header in
					bytes */

#define	LOG_BLOCK_KEY		4	/* encryption key version
					before LOG_BLOCK_CHECKSUM;
					after log_t::FORMAT_ENC_10_4 only */
#define	LOG_BLOCK_CHECKSUM	4	/* 4 byte checksum of the log block
					contents; in InnoDB versions
					< 3.23.52 this did not contain the
					checksum but the same value as
					LOG_BLOCK_HDR_NO */

/** Offsets inside the checkpoint pages (redo log format version 1) @{ */
/** Checkpoint number */
#define LOG_CHECKPOINT_NO		0
/** Log sequence number up to which all changes have been flushed */
#define LOG_CHECKPOINT_LSN		8
/** Byte offset of the log record corresponding to LOG_CHECKPOINT_LSN */
#define LOG_CHECKPOINT_OFFSET		16
/** srv_log_buffer_size at the time of the checkpoint (not used) */
#define LOG_CHECKPOINT_LOG_BUF_SIZE	24
/** MariaDB 10.2.5 encrypted redo log encryption key version (32 bits)*/
#define LOG_CHECKPOINT_CRYPT_KEY	32
/** MariaDB 10.2.5 encrypted redo log random nonce (32 bits) */
#define LOG_CHECKPOINT_CRYPT_NONCE	36
/** MariaDB 10.2.5 encrypted redo log random message (MY_AES_BLOCK_SIZE) */
#define LOG_CHECKPOINT_CRYPT_MESSAGE	40
/** start LSN of the MLOG_CHECKPOINT mini-transaction corresponding
to this checkpoint, or 0 if the information has not been written */
#define LOG_CHECKPOINT_END_LSN		OS_FILE_LOG_BLOCK_SIZE - 16

/* @} */

/** Offsets of a log file header */
/* @{ */
/** Log file header format identifier (32-bit unsigned big-endian integer).
This used to be called LOG_GROUP_ID and always written as 0,
because InnoDB never supported more than one copy of the redo log. */
#define LOG_HEADER_FORMAT	0
/** Redo log subformat (originally 0). In format version 0, the
LOG_FILE_START_LSN started here, 4 bytes earlier than LOG_HEADER_START_LSN,
which the LOG_FILE_START_LSN was renamed to.
Subformat 1 is for the fully redo-logged TRUNCATE
(no MLOG_TRUNCATE records or extra log checkpoints or log file) */
#define LOG_HEADER_SUBFORMAT	4
/** LSN of the start of data in this log file (with format version 1;
in format version 0, it was called LOG_FILE_START_LSN and at offset 4). */
#define LOG_HEADER_START_LSN	8
/** A null-terminated string which will contain either the string 'ibbackup'
and the creation time if the log file was created by mysqlbackup --restore,
or the MySQL version that created the redo log file. */
#define LOG_HEADER_CREATOR	16
/** End of the log file creator field. */
#define LOG_HEADER_CREATOR_END	(LOG_HEADER_CREATOR + 32)
/** Contents of the LOG_HEADER_CREATOR field */
#define LOG_HEADER_CREATOR_CURRENT		\
	"MariaDB "				\
	IB_TO_STR(MYSQL_VERSION_MAJOR) "."	\
	IB_TO_STR(MYSQL_VERSION_MINOR) "."	\
	IB_TO_STR(MYSQL_VERSION_PATCH)

/* @} */

#define LOG_CHECKPOINT_1	OS_FILE_LOG_BLOCK_SIZE
					/* first checkpoint field in the log
					header; we write alternately to the
					checkpoint fields when we make new
					checkpoints; this field is only defined
					in the first log file of a log */
#define LOG_CHECKPOINT_2	(3 * OS_FILE_LOG_BLOCK_SIZE)
					/* second checkpoint field in the log
					header */
#define LOG_FILE_HDR_SIZE	(4 * OS_FILE_LOG_BLOCK_SIZE)

/** Memory mapped file */
class mapped_file_t
{
public:
  mapped_file_t()= default;
  mapped_file_t(const mapped_file_t &)= delete;
  mapped_file_t &operator=(const mapped_file_t &)= delete;
  mapped_file_t(mapped_file_t &&)= delete;
  mapped_file_t &operator=(mapped_file_t &&)= delete;
  ~mapped_file_t() noexcept;

  dberr_t map(const char *path, bool read_only= false,
              bool nvme= false) noexcept;
  dberr_t unmap() noexcept;
  byte *data() noexcept { return m_area.data(); }

private:
  span<byte> m_area;
};

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
  static constexpr uint32_t FORMAT_3_23 = 0;
  /** The MySQL 5.7.9/MariaDB 10.2.2 log format */
  static constexpr uint32_t FORMAT_10_2 = 1;
  /** The MariaDB 10.3.2 log format.
  To prevent crash-downgrade to earlier 10.2 due to the inability to
  roll back a retroactively introduced TRX_UNDO_RENAME_TABLE undo log record,
  MariaDB 10.2.18 and later will use the 10.3 format, but LOG_HEADER_SUBFORMAT
  1 instead of 0. MariaDB 10.3 will use subformat 0 (5.7-style TRUNCATE) or 2
  (MDEV-13564 backup-friendly TRUNCATE). */
  static constexpr uint32_t FORMAT_10_3 = 103;
  /** The MariaDB 10.4.0 log format. */
  static constexpr uint32_t FORMAT_10_4 = 104;
  /** Encrypted MariaDB redo log */
  static constexpr uint32_t FORMAT_ENCRYPTED = 1U << 31;
  /** The MariaDB 10.4.0 log format (only with innodb_encrypt_log=ON) */
  static constexpr uint32_t FORMAT_ENC_10_4 = FORMAT_10_4 | FORMAT_ENCRYPTED;
  /** The MariaDB 10.5 physical redo log format */
  static constexpr uint32_t FORMAT_10_5 = 0x50485953;
  /** The MariaDB 10.5 physical format (only with innodb_encrypt_log=ON) */
  static constexpr uint32_t FORMAT_ENC_10_5 = FORMAT_10_5 | FORMAT_ENCRYPTED;

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
  /** Log file stuff. Protected by mutex. */
  struct file {
    /** format of the redo log: e.g., FORMAT_10_5 */
    uint32_t				format;
    /** redo log subformat: 0 with separately logged TRUNCATE,
    2 with fully redo-logged TRUNCATE (1 in MariaDB 10.2) */
    uint32_t				subformat;
    /** individual log file size in bytes, including the header */
    lsn_t				file_size;
  private:
    /** lsn used to fix coordinates within the log group */
    lsn_t				lsn;
    /** the byte offset of the above lsn */
    lsn_t				lsn_offset;
    /** log file */
    log_file_t				fd;

  public:
    /** used only in recovery: recovery scan succeeded up to this
    lsn in this log group */
    lsn_t				scanned_lsn;

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
    void write(os_offset_t offset, span<byte> buf);
    /** flushes OS page cache (excluding metadata!) for log file */
    void flush();
    /** closes log file */
    void close_file();

    /** @return whether the redo log is encrypted */
    bool is_encrypted() const { return format & FORMAT_ENCRYPTED; }
    /** @return whether the redo log is in the physical format */
    bool is_physical() const
    { return (format & ~FORMAT_ENCRYPTED) == FORMAT_10_5; }
    /** @return capacity in bytes */
    lsn_t capacity() const{ return file_size - LOG_FILE_HDR_SIZE; }
    /** Calculate the offset of a log sequence number.
    @param[in]	lsn	log sequence number
    @return offset within the log */
    inline lsn_t calc_lsn_offset(lsn_t lsn) const;
    inline lsn_t calc_lsn_offset_old(lsn_t lsn) const;

    /** Set the field values to correspond to a given lsn. */
    void set_fields(lsn_t lsn)
    {
      lsn_t c_lsn_offset = calc_lsn_offset(lsn);
      set_lsn(lsn);
      set_lsn_offset(c_lsn_offset);
    }

    /** Read a log segment to log_sys.buf.
    @param[in,out]	start_lsn	in: read area start,
					out: the last read valid lsn
    @param[in]		end_lsn		read area end
    @return	whether no invalid blocks (e.g checksum mismatch) were found */
    bool read_log_seg(lsn_t* start_lsn, lsn_t end_lsn);

    /** Initialize the redo log buffer. */
    void create();

    /** Close the redo log buffer. */
    void close() { close_file(); }
    void set_lsn(lsn_t a_lsn);
    lsn_t get_lsn() const { return lsn; }
    void set_lsn_offset(lsn_t a_lsn);
    lsn_t get_lsn_offset() const { return lsn_offset; }
  } log;

	/** The fields involved in the log buffer flush @{ */

	size_t		buf_next_to_write;/*!< first offset in the log buffer
					where the byte content may not exist
					written to file, e.g., the start
					offset of a log record catenated
					later; this is advanced when a flush
					operation is completed to all the log
					groups */
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
	ib_uint64_t	next_checkpoint_no;
					/*!< next checkpoint number */
  /** latest completed checkpoint (protected by log_sys.mutex) */
  Atomic_relaxed<lsn_t> last_checkpoint_lsn;
	lsn_t		next_checkpoint_lsn;
					/*!< next checkpoint lsn */
	ulint		n_pending_checkpoint_writes;
					/*!< number of currently pending
					checkpoint writes */

  /** buffer for checkpoint header */
  byte *checkpoint_buf;
	/* @} */

private:
  bool m_initialised;
public:
  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */
  log_t(): m_initialised(false) {}

  /** @return whether the redo log is encrypted */
  bool is_encrypted() const { return(log.is_encrypted()); }
  /** @return whether the redo log is in the physical format */
  bool is_physical() const { return log.is_physical(); }

  bool is_initialised() const { return m_initialised; }

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

  bool has_encryption_key_rotation() const {
    return log.format == FORMAT_ENC_10_4 || log.format == FORMAT_ENC_10_5;
  }

  /** @return the log block header + trailer size */
  unsigned framing_size() const
  {
    return has_encryption_key_rotation()
      ? LOG_BLOCK_HDR_SIZE + LOG_BLOCK_KEY + LOG_BLOCK_CHECKSUM
      : LOG_BLOCK_HDR_SIZE + LOG_BLOCK_CHECKSUM;
  }
  /** @return the log block payload size */
  unsigned payload_size() const
  {
    return has_encryption_key_rotation()
      ? OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_CHECKSUM -
      LOG_BLOCK_KEY
      : OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_CHECKSUM;
  }
  /** @return the log block trailer offset */
  unsigned trailer_offset() const
  {
    return has_encryption_key_rotation()
      ? OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_CHECKSUM - LOG_BLOCK_KEY
      : OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_CHECKSUM;
  }

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
};

/** Redo log system */
extern log_t	log_sys;
#ifdef UNIV_DEBUG
extern bool log_write_lock_own();
#endif

/** Calculate the offset of a log sequence number.
@param[in]     lsn     log sequence number
@return offset within the log */
inline lsn_t log_t::file::calc_lsn_offset(lsn_t lsn) const
{
  ut_ad(this == &log_sys.log);
  /* The lsn parameters are updated while holding both the mutexes
  and it is ok to have either of them while reading */
#ifdef SAFE_MUTEX
  ut_ad(mysql_mutex_is_owner(&log_sys.mutex) || log_write_lock_own());
#endif /* SAFE_MUTEX */
  const lsn_t size = capacity();
  lsn_t l= lsn - this->lsn;
  if (longlong(l) < 0) {
	  l = lsn_t(-longlong(l)) % size;
	  l = size - l;
  }

  l+= lsn_offset - LOG_FILE_HDR_SIZE * (1 + lsn_offset / file_size);
  l %= size;
  return l + LOG_FILE_HDR_SIZE * (1 + l / (file_size - LOG_FILE_HDR_SIZE));
}

inline void log_t::file::set_lsn(lsn_t a_lsn)
{
#ifdef SAFE_MUTEX
  ut_ad(mysql_mutex_is_owner(&log_sys.mutex) || log_write_lock_own());
#endif /* SAFE_MUTEX */
  lsn= a_lsn;
}

inline void log_t::file::set_lsn_offset(lsn_t a_lsn)
{
#ifdef SAFE_MUTEX
  ut_ad(mysql_mutex_is_owner(&log_sys.mutex) || log_write_lock_own());
#endif /* SAFE_MUTEX */
  ut_ad((lsn % OS_FILE_LOG_BLOCK_SIZE) == (a_lsn % OS_FILE_LOG_BLOCK_SIZE));
  lsn_offset= a_lsn;
}

#include "log0log.ic"

#endif
