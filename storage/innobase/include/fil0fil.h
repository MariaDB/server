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
@file include/fil0fil.h
The low-level file system

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#ifndef fil0fil_h
#define fil0fil_h

#include "fsp0types.h"
#include "mach0data.h"
#include "assume_aligned.h"

#ifndef UNIV_INNOCHECKSUM

#include "srw_lock.h"
#include "buf0dblwr.h"
#include "hash0hash.h"
#include "log0recv.h"
#include "dict0types.h"
#include "ilist.h"
#include <set>
#include <mutex>

struct unflushed_spaces_tag_t;
struct default_encrypt_tag_t;
struct space_list_tag_t;
struct named_spaces_tag_t;

using space_list_t= ilist<fil_space_t, space_list_tag_t>;

// Forward declaration
extern my_bool srv_use_doublewrite_buf;

/** Possible values of innodb_flush_method */
enum srv_flush_t
{
  /** fsync, the default */
  SRV_FSYNC= 0,
  /** open log files in O_DSYNC mode */
  SRV_O_DSYNC,
  /** do not call os_file_flush() when writing data files, but do flush
  after writing to log files */
  SRV_LITTLESYNC,
  /** do not flush after writing */
  SRV_NOSYNC,
  /** invoke os_file_set_nocache() on data files. This implies using
  unbuffered I/O but still fdatasync(), because some filesystems might
  not flush meta-data on write completion */
  SRV_O_DIRECT,
  /** Like O_DIRECT, but skip fdatasync(), assuming that the data is
  durable on write completion */
  SRV_O_DIRECT_NO_FSYNC
#ifdef _WIN32
  /** Traditional Windows appoach to open all files without caching,
  and do FileFlushBuffers() */
  ,SRV_ALL_O_DIRECT_FSYNC
#endif
};

/** innodb_flush_method */
extern ulong srv_file_flush_method;

/** Undo tablespaces starts with space_id. */
extern uint32_t srv_undo_space_id_start;
/** The number of UNDO tablespaces that are open and ready to use. */
extern uint32_t srv_undo_tablespaces_open;

/** Check whether given space id is undo tablespace id
@param[in]	space_id	space id to check
@return true if it is undo tablespace else false. */
inline bool srv_is_undo_tablespace(uint32_t space_id)
{
  return srv_undo_space_id_start > 0 &&
    space_id >= srv_undo_space_id_start &&
    space_id < srv_undo_space_id_start + srv_undo_tablespaces_open;
}

class page_id_t;

/** Structure containing encryption specification */
struct fil_space_crypt_t;

/** File types */
enum fil_type_t {
	/** temporary tablespace (temporary undo log or tables) */
	FIL_TYPE_TEMPORARY,
	/** a tablespace that is being imported (no logging until finished) */
	FIL_TYPE_IMPORT,
	/** persistent tablespace (for system, undo log or tables) */
	FIL_TYPE_TABLESPACE,
};

struct fil_node_t;

/** Structure to store first and last value of range */
struct range_t
{
  uint32_t first;
  uint32_t last;
};

/** Sort the range based on first value of the range */
struct range_compare
{
  bool operator() (const range_t lhs, const range_t rhs) const
  {
    return lhs.first < rhs.first;
  }
};

using range_set_t= std::set<range_t, range_compare>;
/** Range to store the set of ranges of integers */
class range_set
{
private:
  range_set_t ranges;

  range_set_t::iterator find(uint32_t value) const
  {
    auto r_offset= ranges.lower_bound({value, value});
    const auto r_end= ranges.end();
    if (r_offset != r_end);
    else if (empty())
      return r_end;
    else
      r_offset= std::prev(r_end);
    if (r_offset->first <= value && r_offset->last >= value)
      return r_offset;
    return r_end;
  }
public:
  /** Merge the current range with previous range.
  @param[in] range      range to be merged
  @param[in] prev_range range to be merged with next */
  void merge_range(range_set_t::iterator range,
		   range_set_t::iterator prev_range)
  {
    if (range->first != prev_range->last + 1)
      return;

    /* Merge the current range with previous range */
    range_t new_range {prev_range->first, range->last};
    ranges.erase(prev_range);
    ranges.erase(range);
    ranges.emplace(new_range);
  }

  /** Split the range and add two more ranges
  @param[in] range	range to be split
  @param[in] value	Value to be removed from range */
  void split_range(range_set_t::iterator range, uint32_t value)
  {
    range_t split1{range->first, value - 1};
    range_t split2{value + 1, range->last};

    /* Remove the existing element */
    ranges.erase(range);

    /* Insert the two elements */
    ranges.emplace(split1);
    ranges.emplace(split2);
  }

  /** Remove the value with the given range
  @param[in,out] range  range to be changed
  @param[in]	 value	value to be removed */
  void remove_within_range(range_set_t::iterator range, uint32_t value)
  {
    range_t new_range{range->first, range->last};
    if (value == range->first)
    {
      if (range->first == range->last)
      {
        ranges.erase(range);
        return;
      }
      else
        new_range.first++;
    }
    else if (value == range->last)
      new_range.last--;
    else if (range->first < value && range->last > value)
      return split_range(range, value);

    ranges.erase(range);
    ranges.emplace(new_range);
  }

  /** Remove the value from the ranges.
  @param[in]	value	Value to be removed. */
  void remove_value(uint32_t value)
  {
    if (empty())
      return;
    range_t new_range {value, value};
    range_set_t::iterator range= ranges.lower_bound(new_range);
    if (range == ranges.end())
      return remove_within_range(std::prev(range), value);

    if (range->first > value && range != ranges.begin())
      /* Iterate the previous ranges to delete */
      return remove_within_range(std::prev(range), value);
    return remove_within_range(range, value);
  }
  /** Add the value within the existing range
  @param[in]	range	range to be modified
  @param[in]	value	value to be added */
  range_set_t::iterator add_within_range(range_set_t::iterator range,
                                         uint32_t value)
  {
    if (range->first <= value && range->last >= value)
      return range;

    range_t new_range{range->first, range->last};
    if (range->last + 1 == value)
      new_range.last++;
    else if (range->first - 1 == value)
      new_range.first--;
    else return ranges.end();
    ranges.erase(range);
    return ranges.emplace(new_range).first;
  }
  /** Add the range in the ranges set
  @param[in]	new_range	range to be added */
  void add_range(range_t new_range)
  {
    auto r_offset= ranges.lower_bound(new_range);
    auto r_begin= ranges.begin();
    auto r_end= ranges.end();
    if (!ranges.size())
    {
new_range:
      ranges.emplace(new_range);
      return;
    }

    if (r_offset == r_end)
    {
      /* last range */
      if (add_within_range(std::prev(r_offset), new_range.first) == r_end)
        goto new_range;
    }
    else if (r_offset == r_begin)
    {
      /* First range */
      if (add_within_range(r_offset, new_range.first) == r_end)
        goto new_range;
    }
    else if (r_offset->first - 1 == new_range.first)
    {
      /* Change starting of the existing range */
      auto r_value= add_within_range(r_offset, new_range.first);
      if (r_value != ranges.begin())
        merge_range(r_value, std::prev(r_value));
    }
    else
    {
      /* previous range last_value alone */
      if (add_within_range(std::prev(r_offset), new_range.first) == r_end)
        goto new_range;
    }
  }

 /** Add the value in the ranges
 @param[in] value  value to be added */
  void add_value(uint32_t value)
  {
    range_t new_range{value, value};
    add_range(new_range);
  }

  bool remove_if_exists(uint32_t value)
  {
    auto r_offset= find(value);
    if (r_offset != ranges.end())
    {
      remove_within_range(r_offset, value);
      return true;
    }
    return false;
  }

  bool contains(uint32_t value) const
  {
    return find(value) != ranges.end();
  }

  ulint size() { return ranges.size(); }
  void clear() { ranges.clear(); }
  bool empty() const { return ranges.empty(); }
  typename range_set_t::iterator begin() { return ranges.begin(); }
  typename range_set_t::iterator end() { return ranges.end(); }
};
#endif

/** Tablespace or log data space */
#ifndef UNIV_INNOCHECKSUM
struct fil_io_t
{
  /** error code */
  dberr_t err;
  /** file; node->space->release() must follow IORequestRead call */
  fil_node_t *node;
};

/** Tablespace encryption mode */
enum fil_encryption_t
{
  /** Encrypted if innodb_encrypt_tables=ON (srv_encrypt_tables) */
  FIL_ENCRYPTION_DEFAULT,
  /** Encrypted */
  FIL_ENCRYPTION_ON,
  /** Not encrypted */
  FIL_ENCRYPTION_OFF
};

struct fil_space_t final : ilist_node<unflushed_spaces_tag_t>,
                           ilist_node<default_encrypt_tag_t>,
                           ilist_node<space_list_tag_t>,
                           ilist_node<named_spaces_tag_t>
#else
struct fil_space_t final
#endif
{
#ifndef UNIV_INNOCHECKSUM
  friend fil_node_t;
  ~fil_space_t()
  {
    ut_ad(!latch_owner);
    ut_ad(!latch_count);
    latch.destroy();
  }

  /** fil_system.spaces chain node */
  fil_space_t *hash;
  /** LSN of the most recent fil_names_write_if_was_clean().
  Reset to 0 by fil_names_clear(). Protected by exclusive log_sys.latch.
  If and only if max_lsn is nonzero, this is in fil_system.named_spaces. */
  lsn_t max_lsn;
  /** tablespace identifier */
  uint32_t id;
	/** whether undo tablespace truncation is in progress */
	bool		is_being_truncated;
	fil_type_t	purpose;/*!< purpose */
	UT_LIST_BASE_NODE_T(fil_node_t) chain;
				/*!< base node for the file chain */
	uint32_t	size;	/*!< tablespace file size in pages;
				0 if not known yet */
	uint32_t	size_in_header;
				/* FSP_SIZE in the tablespace header;
				0 if not known yet */
	uint32_t	free_len;
				/*!< length of the FSP_FREE list */
	uint32_t	free_limit;
				/*!< contents of FSP_FREE_LIMIT */
	uint32_t	recv_size;
				/*!< recovered tablespace size in pages;
				0 if no size change was read from the redo log,
				or if the size change was implemented */
	uint32_t	n_reserved_extents;
				/*!< number of reserved free extents for
				ongoing operations like B-tree page split */
private:
#ifdef UNIV_DEBUG
  fil_space_t *next_in_space_list();
  fil_space_t *prev_in_space_list();

  fil_space_t *next_in_unflushed_spaces();
  fil_space_t *prev_in_unflushed_spaces();
#endif

  /** the committed size of the tablespace in pages */
  Atomic_relaxed<uint32_t> committed_size;
  /** Number of pending operations on the file.
  The tablespace cannot be freed while (n_pending & PENDING) != 0. */
  std::atomic<uint32_t> n_pending;
  /** Flag in n_pending that indicates that the tablespace is being
  deleted, and no further operations should be performed */
  static constexpr uint32_t STOPPING= 1U << 31;
  /** Flag in n_pending that indicates that the tablespace is a candidate
  for being closed, and fil_node_t::is_open() can only be trusted after
  acquiring fil_system.mutex and resetting the flag */
  static constexpr uint32_t CLOSING= 1U << 30;
  /** Flag in n_pending that indicates that the tablespace needs fsync().
  This must be the least significant flag bit; @see release_flush() */
  static constexpr uint32_t NEEDS_FSYNC= 1U << 29;
  /** The reference count */
  static constexpr uint32_t PENDING= ~(STOPPING | CLOSING | NEEDS_FSYNC);
  /** latch protecting all page allocation bitmap pages */
  srw_lock latch;
  pthread_t latch_owner;
  ut_d(Atomic_relaxed<uint32_t> latch_count;)
public:
	/** MariaDB encryption data */
	fil_space_crypt_t* crypt_data;

	/** Checks that this tablespace in a list of unflushed tablespaces. */
	bool is_in_unflushed_spaces;

	/** Checks that this tablespace needs key rotation. */
	bool is_in_default_encrypt;

	/** mutex to protect freed ranges */
	std::mutex	freed_range_mutex;

	/** Variables to store freed ranges. This can be used to write
	zeroes/punch the hole in files. Protected by freed_mutex */
	range_set	freed_ranges;

	/** Stores last page freed lsn. Protected by freed_mutex */
	lsn_t		last_freed_lsn;

	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */

  /** @return whether doublewrite buffering is needed */
  inline bool use_doublewrite() const;

	/** Append a file to the chain of files of a space.
	@param[in]	name		file name of a file that is not open
	@param[in]	handle		file handle, or OS_FILE_CLOSED
	@param[in]	size		file size in entire database pages
	@param[in]	is_raw		whether this is a raw device
	@param[in]	atomic_write	true if atomic write could be enabled
	@param[in]	max_pages	maximum number of pages in file,
	or UINT32_MAX for unlimited
	@return file object */
	fil_node_t* add(const char* name, pfs_os_file_t handle,
			uint32_t size, bool is_raw, bool atomic_write,
			uint32_t max_pages = UINT32_MAX);
#ifdef UNIV_DEBUG
	/** Assert that the mini-transaction is compatible with
	updating an allocation bitmap page.
	@param[in]	mtr	mini-transaction */
	void modify_check(const mtr_t& mtr) const;
#endif /* UNIV_DEBUG */

	/** Try to reserve free extents.
	@param[in]	n_free_now	current number of free extents
	@param[in]	n_to_reserve	number of extents to reserve
	@return	whether the reservation succeeded */
	bool reserve_free_extents(uint32_t n_free_now, uint32_t n_to_reserve)
	{
		if (n_reserved_extents + n_to_reserve > n_free_now) {
			return false;
		}

		n_reserved_extents += n_to_reserve;
		return true;
	}

	/** Release the reserved free extents.
	@param[in]	n_reserved	number of reserved extents */
	void release_free_extents(uint32_t n_reserved)
	{
		if (!n_reserved) return;
		ut_a(n_reserved_extents >= n_reserved);
		n_reserved_extents -= n_reserved;
	}

  /** Rename a file.
  @param[in]	path	tablespace file name after renaming
  @param[in]	log	whether to write redo log
  @param[in]	replace	whether to ignore the existence of path
  @return	error code
  @retval	DB_SUCCESS	on success */
  dberr_t rename(const char *path, bool log, bool replace= false)
    MY_ATTRIBUTE((nonnull));

  /** Note that the tablespace has been imported.
  Initially, purpose=FIL_TYPE_IMPORT so that no redo log is
  written while the space ID is being updated in each page. */
  inline void set_imported();

  /** @return whether the storage device is rotational (HDD, not SSD) */
  inline bool is_rotational() const;

  /** whether the tablespace discovery is being deferred during crash
  recovery due to incompletely written page 0 */
  inline bool is_deferred() const;

  /** Open each file. Never invoked on .ibd files.
  @param create_new_db    whether to skip the call to fil_node_t::read_page0()
  @return whether all files were opened */
  bool open(bool create_new_db);
  /** Close each file. Only invoked on fil_system.temp_space. */
  void close();

  /** Note that operations on the tablespace must stop.
  @return whether the operations were already stopped */
  inline bool set_stopping_check();
  /** Note that operations on the tablespace must stop. */
  inline void set_stopping();

  /** Note that operations on the tablespace can resume after truncation */
  inline void clear_stopping();

  /** Look up the tablespace and wait for pending operations to cease
  @param id  tablespace identifier
  @return tablespace
  @retval nullptr if no tablespace was found */
  static fil_space_t *check_pending_operations(uint32_t id);

private:
  MY_ATTRIBUTE((warn_unused_result))
  /** Try to acquire a tablespace reference.
  @return the old reference count (if STOPPING is set, it was not acquired) */
  uint32_t acquire_low()
  {
    uint32_t n= 0;
    while (!n_pending.compare_exchange_strong(n, n + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed) &&
           !(n & STOPPING));
    return n;
  }
public:
  MY_ATTRIBUTE((warn_unused_result))
  /** Acquire a tablespace reference.
  @return whether a tablespace reference was successfully acquired */
  inline bool acquire_if_not_stopped();

  MY_ATTRIBUTE((warn_unused_result))
  /** Acquire a tablespace reference for I/O.
  @return whether the file is usable */
  bool acquire()
  {
    uint32_t n= acquire_low();
    if (UNIV_LIKELY(!(n & (STOPPING | CLOSING))))
      return true;
    return UNIV_LIKELY(!(n & STOPPING)) && prepare();
  }

  /** Acquire another tablespace reference for I/O. */
  inline void reacquire();

  /** Release a tablespace reference.
  @return whether this was the last reference */
  bool release()
  {
    uint32_t n= n_pending.fetch_sub(1, std::memory_order_release);
    ut_ad(n & PENDING);
    return (n & PENDING) == 1;
  }

  /** Clear the NEEDS_FSYNC flag */
  void clear_flush()
  {
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
    static_assert(NEEDS_FSYNC == 1U << 29, "compatibility");
    __asm__ __volatile__("lock btrl $29, %0" : "+m" (n_pending));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
    static_assert(NEEDS_FSYNC == 1U << 29, "compatibility");
    _interlockedbittestandreset(reinterpret_cast<volatile long*>
                                (&n_pending), 29);
#else
    n_pending.fetch_and(~NEEDS_FSYNC, std::memory_order_release);
#endif
  }

private:
  /** Clear the CLOSING flag */
  void clear_closing()
  {
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
    static_assert(CLOSING == 1U << 30, "compatibility");
    __asm__ __volatile__("lock btrl $30, %0" : "+m" (n_pending));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
    static_assert(CLOSING == 1U << 30, "compatibility");
    _interlockedbittestandreset(reinterpret_cast<volatile long*>
                                (&n_pending), 30);
#else
    n_pending.fetch_and(~CLOSING, std::memory_order_relaxed);
#endif
  }

  /** @return pending operations (and flags) */
  uint32_t pending()const { return n_pending.load(std::memory_order_acquire); }
public:
  /** @return whether close() of the file handle has been requested */
  bool is_closing() const { return pending() & CLOSING; }
  /** @return whether the tablespace is going to be dropped */
  bool is_stopping() const { return pending() & STOPPING; }
  /** @return number of pending operations */
  bool is_ready_to_close() const
  { return (pending() & (PENDING | CLOSING)) == CLOSING; }
  /** @return whether fsync() or similar is needed */
  bool needs_flush() const { return pending() & NEEDS_FSYNC; }
  /** @return whether fsync() or similar is needed, and the tablespace is
  not being dropped  */
  bool needs_flush_not_stopping() const
  { return (pending() & (NEEDS_FSYNC | STOPPING)) == NEEDS_FSYNC; }

  uint32_t referenced() const { return pending() & PENDING; }
private:
  MY_ATTRIBUTE((warn_unused_result))
  /** Prepare to close the file handle.
  @return number of pending operations, possibly with NEEDS_FSYNC flag */
  uint32_t set_closing()
  {
    return n_pending.fetch_or(CLOSING, std::memory_order_acquire) &
      (PENDING | NEEDS_FSYNC);
  }

public:
  /** Try to close a file to adhere to the innodb_open_files limit.
  @param print_info   whether to diagnose why a file cannot be closed
  @return whether a file was closed */
  static bool try_to_close(bool print_info);

  /** Close all tablespace files at shutdown */
  static void close_all();

  /** @return last_freed_lsn */
  lsn_t get_last_freed_lsn() { return last_freed_lsn; }
  /** Update last_freed_lsn */
  void update_last_freed_lsn(lsn_t lsn)
  {
    std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
    last_freed_lsn= lsn;
  }

  /** Note that the file will need fsync().
  @return whether this needs to be added to fil_system.unflushed_spaces */
  bool set_needs_flush()
  {
    uint32_t n= 1;
    while (!n_pending.compare_exchange_strong(n, n | NEEDS_FSYNC,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
    {
      ut_ad(n & PENDING);
      if (n & (NEEDS_FSYNC | STOPPING))
        return false;
    }

    return true;
  }

  /** Clear all freed ranges for undo tablespace when InnoDB
  encounters TRIM redo log record */
  void clear_freed_ranges()
  {
    std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
    freed_ranges.clear();
  }
#endif /* !UNIV_INNOCHECKSUM */
  /** FSP_SPACE_FLAGS and FSP_FLAGS_MEM_ flags;
  check fsp0types.h to more info about flags. */
  uint32_t flags;

  /** Determine if full_crc32 is used for a data file
  @param[in]	flags	tablespace flags (FSP_SPACE_FLAGS)
  @return whether the full_crc32 algorithm is active */
  static bool full_crc32(uint32_t flags)
  { return flags & FSP_FLAGS_FCRC32_MASK_MARKER; }
  /** @return whether innodb_checksum_algorithm=full_crc32 is active */
  bool full_crc32() const { return full_crc32(flags); }
  /** Determine if full_crc32 is used along with PAGE_COMPRESSED */
  static bool is_full_crc32_compressed(uint32_t flags)
  {
    if (!full_crc32(flags))
      return false;
    auto algo= FSP_FLAGS_FCRC32_GET_COMPRESSED_ALGO(flags);
    DBUG_ASSERT(algo <= PAGE_ALGORITHM_LAST);
    return algo != 0;
  }
  /** Determine the logical page size.
  @param flags	tablespace flags (FSP_SPACE_FLAGS)
  @return the logical page size
  @retval 0 if the flags are invalid */
  static unsigned logical_size(uint32_t flags)
  {
    switch (full_crc32(flags)
            ? FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(flags)
            : FSP_FLAGS_GET_PAGE_SSIZE(flags)) {
    case 3: return 4096;
    case 4: return 8192;
    case 5: return full_crc32(flags) ? 16384 : 0;
    case 0: return full_crc32(flags) ? 0 : 16384;
    case 6: return 32768;
    case 7: return 65536;
    default: return 0;
    }
  }
  /** Determine the ROW_FORMAT=COMPRESSED page size.
  @param flags	tablespace flags (FSP_SPACE_FLAGS)
  @return the ROW_FORMAT=COMPRESSED page size
  @retval 0	if ROW_FORMAT=COMPRESSED is not used */
  static unsigned zip_size(uint32_t flags)
  {
    if (full_crc32(flags))
      return 0;
    const uint32_t zip_ssize= FSP_FLAGS_GET_ZIP_SSIZE(flags);
    return zip_ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << zip_ssize : 0;
  }
  /** Determine the physical page size.
  @param flags	tablespace flags (FSP_SPACE_FLAGS)
  @return the physical page size */
  static unsigned physical_size(uint32_t flags)
  {
    if (full_crc32(flags))
      return logical_size(flags);

    const uint32_t zip_ssize= FSP_FLAGS_GET_ZIP_SSIZE(flags);
    return zip_ssize
      ? (UNIV_ZIP_SIZE_MIN >> 1) << zip_ssize
      : unsigned(srv_page_size);
  }

  /** @return the ROW_FORMAT=COMPRESSED page size
  @retval 0  if ROW_FORMAT=COMPRESSED is not used */
  unsigned zip_size() const { return zip_size(flags); }
  /** @return the physical page size */
  unsigned physical_size() const { return physical_size(flags); }

  /** Check whether PAGE_COMPRESSED is enabled.
  @param[in]	flags	tablespace flags */
  static bool is_compressed(uint32_t flags)
  {
    return is_full_crc32_compressed(flags) ||
      FSP_FLAGS_HAS_PAGE_COMPRESSION(flags);
  }
  /** @return whether the compression enabled for the tablespace. */
  bool is_compressed() const { return is_compressed(flags); }

  /** Get the compression algorithm for full crc32 format.
  @param flags contents of FSP_SPACE_FLAGS
  @return PAGE_COMPRESSED algorithm of full_crc32 tablespace
  @retval 0 if not PAGE_COMPRESSED or not full_crc32 */
  static unsigned get_compression_algo(uint32_t flags)
  {
    return full_crc32(flags)
      ? FSP_FLAGS_FCRC32_GET_COMPRESSED_ALGO(flags)
      : 0;
  }
  /** @return the page_compressed algorithm
  @retval 0 if not page_compressed */
  unsigned get_compression_algo() const { return get_compression_algo(flags); }
  /** Determine if the page_compressed page contains an extra byte
  for exact compressed stream length
  @param flags   contents of FSP_SPACE_FLAGS
  @return whether the extra byte is needed */
  static bool full_crc32_page_compressed_len(uint32_t flags)
  {
    DBUG_ASSERT(full_crc32(flags));
    switch (get_compression_algo(flags)) {
    case PAGE_LZ4_ALGORITHM:
    case PAGE_LZO_ALGORITHM:
    case PAGE_SNAPPY_ALGORITHM:
      return true;
    }
    return false;
  }

  /** Whether the full checksum matches with non full checksum flags.
  @param flags    contents of FSP_SPACE_FLAGS
  @param expected expected flags
  @return true if it is equivalent */
  static bool is_flags_full_crc32_equal(uint32_t flags, uint32_t expected)
  {
    ut_ad(full_crc32(flags));
    uint32_t fcrc32_psize= FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(flags);

    if (full_crc32(expected))
      /* The data file may have been created with a
      different innodb_compression_algorithm. But
      we only support one innodb_page_size for all files. */
      return fcrc32_psize == FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(expected);

    uint32_t non_fcrc32_psize = FSP_FLAGS_GET_PAGE_SSIZE(expected);
    if (!non_fcrc32_psize)
      return fcrc32_psize == 5;
    return fcrc32_psize == non_fcrc32_psize;
  }

  /** Whether old tablespace flags match full_crc32 flags.
  @param flags    contents of FSP_SPACE_FLAGS
  @param expected expected flags
  @return true if it is equivalent */
  static bool is_flags_non_full_crc32_equal(uint32_t flags, uint32_t expected)
  {
    ut_ad(!full_crc32(flags));
    if (!full_crc32(expected))
      return false;

    uint32_t non_fcrc32_psize= FSP_FLAGS_GET_PAGE_SSIZE(flags);
    uint32_t fcrc32_psize = FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(expected);

    if (!non_fcrc32_psize)
      return fcrc32_psize == 5;
    return fcrc32_psize == non_fcrc32_psize;
  }

  /** Whether both fsp flags are equivalent */
  static bool is_flags_equal(uint32_t flags, uint32_t expected)
  {
    if (!((flags ^ expected) & ~(1U << FSP_FLAGS_POS_RESERVED)))
      return true;
    return full_crc32(flags)
       ? is_flags_full_crc32_equal(flags, expected)
       : is_flags_non_full_crc32_equal(flags, expected);
  }

  /** Validate the tablespace flags for full crc32 format.
  @param flags contents of FSP_SPACE_FLAGS
  @return whether the flags are correct in full crc32 format */
  static bool is_fcrc32_valid_flags(uint32_t flags)
  {
    ut_ad(flags & FSP_FLAGS_FCRC32_MASK_MARKER);
    const ulint page_ssize= physical_size(flags);
    if (page_ssize < 3 || page_ssize & 8)
      return false;
    flags >>= FSP_FLAGS_FCRC32_POS_COMPRESSED_ALGO;
    return flags <= PAGE_ALGORITHM_LAST;
  }
  /** Validate the tablespace flags.
  @param flags	contents of FSP_SPACE_FLAGS
  @param is_ibd	whether this is an .ibd file (not system tablespace)
  @return whether the flags are correct */
  static bool is_valid_flags(uint32_t flags, bool is_ibd)
  {
    DBUG_EXECUTE_IF("fsp_flags_is_valid_failure", return false;);
    if (full_crc32(flags))
      return is_fcrc32_valid_flags(flags);

    if (flags == 0)
      return true;
    if (~FSP_FLAGS_MASK & flags)
      return false;

    if (FSP_FLAGS_MASK_ATOMIC_BLOBS ==
        (flags & (FSP_FLAGS_MASK_POST_ANTELOPE | FSP_FLAGS_MASK_ATOMIC_BLOBS)))
      /* If the "atomic blobs" flag (indicating
      ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED) flag is set, then the
      ROW_FORMAT!=REDUNDANT flag must also be set. */
      return false;

    /* Bits 10..14 should be 0b0000d where d is the DATA_DIR flag
    of MySQL 5.6 and MariaDB 10.0, which we ignore.
    In the buggy FSP_SPACE_FLAGS written by MariaDB 10.1.0 to 10.1.20,
    bits 10..14 would be nonzero 0bsssaa where sss is
    nonzero PAGE_SSIZE (3, 4, 6, or 7)
    and aa is ATOMIC_WRITES (not 0b11). */
    if (FSP_FLAGS_GET_RESERVED(flags) & ~1U)
      return false;

    const uint32_t ssize= FSP_FLAGS_GET_PAGE_SSIZE(flags);
    if (ssize == 1 || ssize == 2 || ssize == 5 || ssize & 8)
      /* the page_size is not between 4k and 64k;
      16k should be encoded as 0, not 5 */
      return false;

    const uint32_t zssize= FSP_FLAGS_GET_ZIP_SSIZE(flags);
    if (zssize == 0)
      /* not ROW_FORMAT=COMPRESSED */;
    else if (zssize > (ssize ? ssize : 5))
      /* Invalid KEY_BLOCK_SIZE */
      return false;
    else if (~flags &
             (FSP_FLAGS_MASK_POST_ANTELOPE | FSP_FLAGS_MASK_ATOMIC_BLOBS))
     /* both these flags must set for ROW_FORMAT=COMPRESSED */
     return false;

    /* The flags do look valid. But, avoid misinterpreting
    buggy MariaDB 10.1 format flags for
    PAGE_COMPRESSED=1 PAGE_COMPRESSION_LEVEL={0,2,3}
    as valid-looking PAGE_SSIZE if this is known to be
    an .ibd file and we are using the default innodb_page_size=16k. */
    return(ssize == 0 || !is_ibd || srv_page_size != UNIV_PAGE_SIZE_ORIG);
  }

#ifndef UNIV_INNOCHECKSUM
  MY_ATTRIBUTE((warn_unused_result))
  /** Create a tablespace in fil_system.
  @param id         tablespace identifier
  @param flags      tablespace flags
  @param purpose    tablespace purpose
  @param crypt_data encryption information
  @param mode       encryption mode
  @return pointer to created tablespace, to be filled in with add()
  @retval nullptr on failure (such as when the same tablespace exists) */
  static fil_space_t *create(uint32_t id, uint32_t flags,
                             fil_type_t purpose, fil_space_crypt_t *crypt_data,
                             fil_encryption_t mode= FIL_ENCRYPTION_DEFAULT);

  MY_ATTRIBUTE((warn_unused_result))
  /** Acquire a tablespace reference.
  @param id      tablespace identifier
  @return tablespace
  @retval nullptr if the tablespace is missing or inaccessible */
  static fil_space_t *get(uint32_t id);

  /** Add/remove the free page in the freed ranges list.
  @param[in] offset     page number to be added
  @param[in] free       true if page to be freed */
  void free_page(uint32_t offset, bool add=true)
  {
    std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
    if (add)
      return freed_ranges.add_value(offset);

    if (freed_ranges.empty())
      return;

    return freed_ranges.remove_value(offset);
  }

  /** Add the range of freed pages */
  void add_free_ranges(range_set ranges)
  {
    std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
    freed_ranges= std::move(ranges);
  }

  /** Add the set of freed page ranges */
  void add_free_range(const range_t range)
  {
    std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
    freed_ranges.add_range(range);
  }

  /** Set the tablespace size in pages */
  void set_sizes(uint32_t s)
  {
    ut_ad(id ? !size : (size >= s));
    size= s; committed_size= s;
  }

  /** Update committed_size in mtr_t::commit() */
  void set_committed_size() { committed_size= size; }

  /** @return the last persisted page number */
  uint32_t last_page_number() const { return committed_size - 1; }

  /** @return the size in pages (0 if unreadable) */
  inline uint32_t get_size();

  /** Read or write data.
  @param type     I/O context
  @param offset   offset in bytes
  @param len      number of bytes
  @param buf      the data to be read or written
  @param bpage    buffer block (for type.is_async() completion callback)
  @return status and file descriptor */
  fil_io_t io(const IORequest &type, os_offset_t offset, size_t len,
              void *buf, buf_page_t *bpage= nullptr);
  /** Flush pending writes from the file system cache to the file. */
  template<bool have_reference> inline void flush();
  /** Flush pending writes from the file system cache to the file. */
  void flush_low();

  /** Read the first page of a data file.
  @return whether the page was found valid */
  bool read_page0();

  /** Determine the next tablespace for encryption key rotation.
  @param space    current tablespace (nullptr to start from the beginning)
  @param recheck  whether the removal condition needs to be rechecked after
                  encryption parameters were changed
  @param encrypt  expected state of innodb_encrypt_tables
  @return the next tablespace
  @retval nullptr upon reaching the end of the iteration */
  static space_list_t::iterator next(space_list_t::iterator space,
                                     bool recheck, bool encrypt);

#ifdef UNIV_DEBUG
  bool is_latched() const { return latch_count != 0; }
#endif
  bool is_owner() const { return latch_owner == pthread_self(); }
  /** Acquire the allocation latch in exclusive mode */
  void x_lock()
  {
    latch.wr_lock(SRW_LOCK_CALL);
    ut_ad(!latch_owner);
    latch_owner= pthread_self();
    ut_ad(!latch_count.fetch_add(1));
  }
  /** Release the allocation latch from exclusive mode */
  void x_unlock()
  {
    ut_ad(latch_count.fetch_sub(1) == 1);
    ut_ad(latch_owner == pthread_self());
    latch_owner= 0;
    latch.wr_unlock();
  }
  /** Acquire the allocation latch in shared mode */
  void s_lock()
  {
    ut_ad(!is_owner());
    latch.rd_lock(SRW_LOCK_CALL);
    ut_ad(!latch_owner);
    ut_d(latch_count.fetch_add(1));
  }
  /** Release the allocation latch from shared mode */
  void s_unlock()
  {
    ut_ad(latch_count.fetch_sub(1));
    ut_ad(!latch_owner);
    latch.rd_unlock();
  }

  typedef span<const char> name_type;

  /** @return the tablespace name (databasename/tablename) */
  name_type name() const;

private:
  /** @return whether the file is usable for io() */
  ATTRIBUTE_COLD bool prepare(bool have_mutex= false);
#endif /*!UNIV_INNOCHECKSUM */
};

#ifndef UNIV_INNOCHECKSUM
/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** File node of a tablespace or the log data space */
struct fil_node_t final
{
  /** tablespace containing this file */
  fil_space_t *space;
  /** file name; protected by fil_system.mutex and exclusive log_sys.latch */
  char *name;
  /** file handle */
  pfs_os_file_t handle;
  /** whether the file is on non-rotational media (SSD) */
  unsigned on_ssd:1;
  /** how to write page_compressed tables
  (0=do not punch holes but write minimal amount of data, 1=punch holes,
  2=always write the same amount; thinly provisioned storage will compress) */
  unsigned punch_hole:2;
  /** whether this file could use atomic write */
  unsigned atomic_write:1;
  /** whether the file actually is a raw device or disk partition */
  unsigned is_raw_disk:1;
  /** whether the tablespace discovery is being deferred during crash
  recovery due to incompletely written page 0 */
  unsigned deferred:1;

  /** size of the file in database pages (0 if not known yet);
  the possible last incomplete megabyte may be ignored if space->id == 0 */
  uint32_t size;
  /** initial size of the file in database pages;
  FIL_IBD_FILE_INITIAL_SIZE by default */
  uint32_t init_size;
  /** maximum size of the file in database pages (0 if unlimited) */
  uint32_t max_size;
  /** whether the file is currently being extended */
  Atomic_relaxed<bool> being_extended;
  /** link to other files in this tablespace */
  UT_LIST_NODE_T(fil_node_t) chain;

  /** Filesystem block size */
  ulint block_size;

  /** @return whether this file is open */
  bool is_open() const { return handle != OS_FILE_CLOSED; }

  /** Read the first page of a data file.
  @return whether the page was found valid */
  bool read_page0();

  /** Determine some file metadata when creating or reading the file.
  @param file   the file that is being created, or OS_FILE_CLOSED */
  void find_metadata(os_file_t file= OS_FILE_CLOSED
#ifndef _WIN32
                     , bool create= false, struct stat *statbuf= nullptr
#endif
                     );

  /** Close the file handle. */
  void close();
  /** Same as close() but returns file handle instead of closing it. */
  pfs_os_file_t detach() MY_ATTRIBUTE((warn_unused_result));
  /** Prepare to free a file from fil_system.
  @param detach_handle whether to detach instead of closing a handle
  @return detached handle or OS_FILE_CLOSED */
  inline pfs_os_file_t close_to_free(bool detach_handle= false);

  /** Update the data structures on write completion */
  inline void complete_write();

private:
  /** Does stuff common for close() and detach() */
  void prepare_to_close_or_detach();
};

inline bool fil_space_t::use_doublewrite() const
{
  return !UT_LIST_GET_FIRST(chain)->atomic_write && srv_use_doublewrite_buf &&
    buf_dblwr.is_initialised();
}

inline void fil_space_t::set_imported()
{
  ut_ad(purpose == FIL_TYPE_IMPORT);
  purpose= FIL_TYPE_TABLESPACE;
  UT_LIST_GET_FIRST(chain)->find_metadata();
}

inline bool fil_space_t::is_rotational() const
{
  for (const fil_node_t *node= UT_LIST_GET_FIRST(chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
    if (!node->on_ssd)
      return true;
  return false;
}

inline bool fil_space_t::is_deferred() const
{
  return UT_LIST_GET_FIRST(chain)->deferred;
}

/** Common InnoDB file extensions */
enum ib_extention {
	NO_EXT = 0,
	IBD = 1,
	ISL = 2,
	CFG = 3
};
extern const char* dot_ext[];
#define DOT_IBD dot_ext[IBD]
#define DOT_ISL dot_ext[ISL]
#define DOT_CFG dot_ext[CFG]

/** When mariadbd is run, the default directory "." is the mysqld datadir,
but in the MariaDB Embedded Server Library and mysqlbackup it is not the default
directory, and we must set the base file path explicitly */
extern const char*	fil_path_to_mysql_datadir;
#else
# include "univ.i"
#endif /* !UNIV_INNOCHECKSUM */

/** Initial size of a single-table tablespace in pages */
#define FIL_IBD_FILE_INITIAL_SIZE	4U

/** 'null' (undefined) page offset in the context of file spaces */
#define	FIL_NULL	ULINT32_UNDEFINED


#define FIL_ADDR_PAGE	0U	/* first in address is the page offset */
#define	FIL_ADDR_BYTE	4U	/* then comes 2-byte byte offset within page*/
#define	FIL_ADDR_SIZE	6U	/* address size is 6 bytes */

/** File space address */
struct fil_addr_t {
  /** page number within a tablespace */
  uint32_t page;
  /** byte offset within the page */
  uint16_t boffset;
};

/** The byte offsets on a file page for various variables @{ */
#define FIL_PAGE_SPACE_OR_CHKSUM 0	/*!< in < MySQL-4.0.14 space id the
					page belongs to (== 0) but in later
					versions the 'new' checksum of the
					page */
#define FIL_PAGE_OFFSET		4U	/*!< page offset inside space */
#define FIL_PAGE_PREV		8U	/*!< if there is a 'natural'
					predecessor of the page, its
					offset.  Otherwise FIL_NULL.
					This field is not set on BLOB
					pages, which are stored as a
					singly-linked list.  See also
					FIL_PAGE_NEXT. */
#define FIL_PAGE_NEXT		12U	/*!< if there is a 'natural' successor
					of the page, its offset.
					Otherwise FIL_NULL.
					B-tree index pages
					(FIL_PAGE_TYPE contains FIL_PAGE_INDEX)
					on the same PAGE_LEVEL are maintained
					as a doubly linked list via
					FIL_PAGE_PREV and FIL_PAGE_NEXT
					in the collation order of the
					smallest user record on each page. */
#define FIL_PAGE_LSN		16U	/*!< lsn of the end of the newest
					modification log record to the page */
#define	FIL_PAGE_TYPE		24U	/*!< file page type: FIL_PAGE_INDEX,...,
					2 bytes.

					The contents of this field can only
					be trusted in the following case:
					if the page is an uncompressed
					B-tree index page, then it is
					guaranteed that the value is
					FIL_PAGE_INDEX.
					The opposite does not hold.

					In tablespaces created by
					MySQL/InnoDB 5.1.7 or later, the
					contents of this field is valid
					for all uncompressed pages. */

/** For the first page in a system tablespace data file(ibdata*, not *.ibd):
the file has been flushed to disk at least up to this lsn
For other pages: 32-bit key version used to encrypt the page + 32-bit checksum
or 64 bites of zero if no encryption */
#define FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION 26U

/** This overloads FIL_PAGE_FILE_FLUSH_LSN for RTREE Split Sequence Number */
#define	FIL_RTREE_SPLIT_SEQ_NUM	FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION

/** Start of the page_compressed content */
#define FIL_PAGE_COMP_ALGO	FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION

/** starting from 4.1.x this contains the space id of the page */
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID  34U

#define FIL_PAGE_SPACE_ID  FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID

#define FIL_PAGE_DATA		38U	/*!< start of the data on the page */

/** 32-bit key version used to encrypt the page in full_crc32 format.
For non-encrypted page, it contains 0. */
#define FIL_PAGE_FCRC32_KEY_VERSION	0

/** page_compressed without innodb_checksum_algorithm=full_crc32 @{ */
/** Number of bytes used to store actual payload data size on
page_compressed pages when not using full_crc32. */
#define FIL_PAGE_COMP_SIZE		0

/** Number of bytes for FIL_PAGE_COMP_SIZE */
#define FIL_PAGE_COMP_METADATA_LEN		2

/** Number of bytes used to store actual compression method
for encrypted tables when not using full_crc32. */
#define FIL_PAGE_ENCRYPT_COMP_ALGO		2

/** Extra header size for encrypted page_compressed pages when
not using full_crc32 */
#define FIL_PAGE_ENCRYPT_COMP_METADATA_LEN	4
/* @} */

/** File page trailer @{ */
#define FIL_PAGE_END_LSN_OLD_CHKSUM 8	/*!< the low 4 bytes of this are used
					to store the page checksum, the
					last 4 bytes should be identical
					to the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_DATA_END	8	/*!< size of the page trailer */

/** Store the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_FCRC32_END_LSN 8

/** Store crc32 checksum at the end of the page */
#define FIL_PAGE_FCRC32_CHECKSUM	4
/* @} */

/** File page types (values of FIL_PAGE_TYPE) @{ */
/** page_compressed, encrypted=YES (not used for full_crc32) */
constexpr uint16_t FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED= 37401;
/** page_compressed (not used for full_crc32) */
constexpr uint16_t FIL_PAGE_PAGE_COMPRESSED= 34354;
/** B-tree index page */
constexpr uint16_t FIL_PAGE_INDEX= 17855;
/** R-tree index page (SPATIAL INDEX) */
constexpr uint16_t FIL_PAGE_RTREE= 17854;
/** Undo log page */
constexpr uint16_t FIL_PAGE_UNDO_LOG= 2;
/** Index node (of file-in-file metadata) */
constexpr uint16_t FIL_PAGE_INODE= 3;
/** Insert buffer free list */
constexpr uint16_t FIL_PAGE_IBUF_FREE_LIST= 4;
/** Freshly allocated page */
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED= 0;
/** Change buffer bitmap (pages n*innodb_page_size+1) */
constexpr uint16_t FIL_PAGE_IBUF_BITMAP= 5;
/** System page */
constexpr uint16_t FIL_PAGE_TYPE_SYS= 6;
/** Transaction system data */
constexpr uint16_t FIL_PAGE_TYPE_TRX_SYS= 7;
/** Tablespace header (page 0) */
constexpr uint16_t FIL_PAGE_TYPE_FSP_HDR= 8;
/** Extent descriptor page (pages n*innodb_page_size, except 0) */
constexpr uint16_t FIL_PAGE_TYPE_XDES= 9;
/** Uncompressed BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_BLOB= 10;
/** First ROW_FORMAT=COMPRESSED BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB= 11;
/** Subsequent ROW_FORMAT=COMPRESSED BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB2= 12;
/** In old tablespaces, garbage in FIL_PAGE_TYPE is replaced with this
value when flushing pages. */
constexpr uint16_t FIL_PAGE_TYPE_UNKNOWN= 13;

/* File page types introduced in MySQL 5.7, not supported in MariaDB */
//constexpr uint16_t FIL_PAGE_COMPRESSED = 14;
//constexpr uint16_t FIL_PAGE_ENCRYPTED = 15;
//constexpr uint16_t FIL_PAGE_COMPRESSED_AND_ENCRYPTED = 16;
//constexpr FIL_PAGE_ENCRYPTED_RTREE = 17;
/** Clustered index root page after instant ADD COLUMN */
constexpr uint16_t FIL_PAGE_TYPE_INSTANT= 18;

/** Used by i_s.cc to index into the text description.
Note: FIL_PAGE_TYPE_INSTANT maps to the same as FIL_PAGE_INDEX. */
constexpr uint16_t FIL_PAGE_TYPE_LAST= FIL_PAGE_TYPE_UNKNOWN;

/** Set in FIL_PAGE_TYPE for full_crc32 pages in page_compressed format.
If the flag is set, then the following holds for the remaining bits
of FIL_PAGE_TYPE:
Bits 0..7 will contain the compressed page size in bytes.
Bits 8..14 are reserved and must be 0. */
constexpr uint16_t FIL_PAGE_COMPRESS_FCRC32_MARKER= 15;
/* @} */

/** @return whether the page type is B-tree or R-tree index */
inline bool fil_page_type_is_index(uint16_t page_type)
{
	switch (page_type) {
	case FIL_PAGE_TYPE_INSTANT:
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		return(true);
	}
	return(false);
}

/** Check whether the page is index page (either regular Btree index or Rtree
index */
#define fil_page_index_page_check(page)                         \
        fil_page_type_is_index(fil_page_get_type(page))

/** Get the file page type.
@param[in]	page	file page
@return page type */
inline uint16_t fil_page_get_type(const byte *page)
{
  return mach_read_from_2(my_assume_aligned<2>(page + FIL_PAGE_TYPE));
}

#ifndef UNIV_INNOCHECKSUM

/** Number of pending tablespace flushes */
extern Atomic_counter<ulint> fil_n_pending_tablespace_flushes;

/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
Normally, fil_space_t::get() should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or NULL if not found */
fil_space_t *fil_space_get(uint32_t id)
  MY_ATTRIBUTE((warn_unused_result));

/** The tablespace memory cache */
struct fil_system_t
{
  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */
  fil_system_t() : m_initialised(false) {}

  bool is_initialised() const { return m_initialised; }

  /**
    Create the file system interface at database start.

    @param[in] hash_size	hash table size
  */
  void create(ulint hash_size);

  /** Close the file system interface at shutdown */
  void close();

private:
  bool m_initialised;
#ifdef UNIV_LINUX
  /** available block devices that reside on non-rotational storage */
  std::vector<dev_t> ssd;
public:
  /** @return whether a file system device is on non-rotational storage */
  bool is_ssd(dev_t dev) const
  {
    /* Linux seems to allow up to 15 partitions per block device.
    If the detected ssd carries "partition number 0" (it is the whole device),
    compare the candidate file system number without the partition number. */
    for (const auto s : ssd)
      if (dev == s || (dev & ~15U) == s)
        return true;
    return false;
  }
#endif
public:
  /** Detach a tablespace from the cache and close the files.
  @param space tablespace
  @param detach_handle whether to detach the handle, instead of closing
  @return detached handle
  @retval OS_FILE_CLOSED if no handle was detached */
  pfs_os_file_t detach(fil_space_t *space, bool detach_handle= false);

  /** the mutex protecting most data fields, and some fields of fil_space_t */
  mysql_mutex_t mutex;
	fil_space_t*	sys_space;	/*!< The innodb_system tablespace */
	fil_space_t*	temp_space;	/*!< The innodb_temporary tablespace */
  /** Map of fil_space_t::id to fil_space_t* */
  hash_table_t spaces;
  /** tablespaces for which fil_space_t::needs_flush() holds */
  sized_ilist<fil_space_t, unflushed_spaces_tag_t> unflushed_spaces;
  /** number of currently open files; protected by mutex */
  ulint n_open;
  /** last time we noted n_open exceeding the limit; protected by mutex */
  time_t n_open_exceeded_time;
  /** maximum persistent tablespace id that has ever been assigned */
  uint32_t max_assigned_id;
  /** nonzero if fil_node_open_file_low() should avoid moving the tablespace
  to the end of space_list, for FIFO policy of try_to_close() */
  ulint freeze_space_list;
  /** list of all tablespaces */
  ilist<fil_space_t, space_list_tag_t> space_list;
  /** list of all tablespaces for which a FILE_MODIFY record has been written
  since the latest redo log checkpoint.
  Protected only by exclusive log_sys.latch. */
  ilist<fil_space_t, named_spaces_tag_t> named_spaces;

  /** list of all ENCRYPTED=DEFAULT tablespaces that need
  to be converted to the current value of innodb_encrypt_tables */
  ilist<fil_space_t, default_encrypt_tag_t> default_encrypt_tables;

  /** whether fil_space_t::create() has issued a warning about
  potential space_id reuse */
  bool space_id_reuse_warned;

  /** Return the next tablespace from default_encrypt_tables list.
  @param space   previous tablespace (nullptr to start from the start)
  @param recheck whether the removal condition needs to be rechecked after
  the encryption parameters were changed
  @param encrypt expected state of innodb_encrypt_tables
  @return the next tablespace to process (n_pending_ops incremented)
  @retval fil_system.temp_space if there is no work to do
  @retval nullptr upon reaching the end of the iteration */
  inline fil_space_t* default_encrypt_next(fil_space_t *space, bool recheck,
                                           bool encrypt);

  /** Extend all open data files to the recovered size */
  ATTRIBUTE_COLD void extend_to_recv_size();

  /** Determine if a tablespace associated with a file name exists.
  @param path   tablespace file name to look for
  @return a matching tablespace */
  inline fil_space_t *find(const char *path) const;
};

/** The tablespace memory cache. */
extern fil_system_t	fil_system;

inline void fil_space_t::reacquire()
{
  ut_d(uint32_t n=) n_pending.fetch_add(1, std::memory_order_relaxed);
#ifdef SAFE_MUTEX
  if (mysql_mutex_is_owner(&fil_system.mutex)) return;
  ut_ad(n & PENDING);
  ut_ad(UT_LIST_GET_FIRST(chain)->is_open());
#endif /* SAFE_MUTEX */
}

/** Note that operations on the tablespace must stop.
@return whether the operations were already stopped */
inline bool fil_space_t::set_stopping_check()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
#if (defined __clang_major__ && __clang_major__ < 10) || defined __APPLE_CC__
  /* Only clang-10 introduced support for asm goto */
  return n_pending.fetch_or(STOPPING, std::memory_order_relaxed) & STOPPING;
#elif defined __GNUC__ && (defined __i386__ || defined __x86_64__)
  static_assert(STOPPING == 1U << 31, "compatibility");
  __asm__ goto("lock btsl $31, %0\t\njnc %l1" : : "m" (n_pending)
               : "cc", "memory" : not_stopped);
  return true;
not_stopped:
  return false;
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
  static_assert(STOPPING == 1U << 31, "compatibility");
  return _interlockedbittestandset(reinterpret_cast<volatile long*>
                                   (&n_pending), 31);
#else
  return n_pending.fetch_or(STOPPING, std::memory_order_relaxed) & STOPPING;
#endif
}

/** Note that operations on the tablespace must stop.
@return whether the operations were already stopped */
inline void fil_space_t::set_stopping()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
  static_assert(STOPPING == 1U << 31, "compatibility");
  __asm__ __volatile__("lock btsl $31, %0" : "+m" (n_pending));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
  static_assert(STOPPING == 1U << 31, "compatibility");
  _interlockedbittestandset(reinterpret_cast<volatile long*>(&n_pending), 31);
#else
  n_pending.fetch_or(STOPPING, std::memory_order_relaxed);
#endif
}

inline void fil_space_t::clear_stopping()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  static_assert(STOPPING == 1U << 31, "compatibility");
  ut_d(auto n=) n_pending.fetch_sub(STOPPING, std::memory_order_relaxed);
  ut_ad(n & STOPPING);
}

/** Flush pending writes from the file system cache to the file. */
template<bool have_reference> inline void fil_space_t::flush()
{
  mysql_mutex_assert_not_owner(&fil_system.mutex);
  ut_ad(!have_reference || (pending() & PENDING));
  ut_ad(purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_IMPORT);
  if (srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC)
  {
    ut_ad(!is_in_unflushed_spaces);
    ut_ad(!needs_flush());
  }
  else if (have_reference)
    flush_low();
  else if (!(acquire_low() & STOPPING))
  {
    flush_low();
    release();
  }
}

/** @return the size in pages (0 if unreadable) */
inline uint32_t fil_space_t::get_size()
{
  if (!size)
  {
    mysql_mutex_lock(&fil_system.mutex);
    read_page0();
    mysql_mutex_unlock(&fil_system.mutex);
  }
  return size;
}

#include "fil0crypt.h"

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */
bool fil_assign_new_space_id(uint32_t *space_id);

/** Frees a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param id          tablespace identifier
@param x_latched   whether the caller holds exclusive fil_space_t::latch
@return true if success */
bool fil_space_free(uint32_t id, bool x_latched);

/** Set the recovered size of a tablespace in pages.
@param	id	tablespace ID
@param	size	recovered size in pages
@param	flags	tablespace flags */
void fil_space_set_recv_size_and_flags(uint32_t id, uint32_t size,
                                       uint32_t flags);

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
void fil_set_max_space_id_if_bigger(uint32_t max_id);

MY_ATTRIBUTE((warn_unused_result))
/** Delete a tablespace and associated .ibd file.
@param id    tablespace identifier
@return detached file handle (to be closed by the caller)
@return	OS_FILE_CLOSED if no file existed */
pfs_os_file_t fil_delete_tablespace(uint32_t id);

/** Close a single-table tablespace on failed IMPORT TABLESPACE.
The tablespace must be cached in the memory cache.
Free all pages used by the tablespace. */
void fil_close_tablespace(uint32_t id);

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the directory path or the full path and filename.
@param[in] name {} if path is full, or Table/Tablespace name
@param[in] ext the file extension to use
@param[in] trim_name true if the last name on the path should be trimmed.
@return own: file name */
char* fil_make_filepath(const char *path, const fil_space_t::name_type &name,
                        ib_extention ext, bool trim_name);

char *fil_make_filepath(const char* path, const table_name_t name,
                        ib_extention suffix, bool strip_name);

/** Create a tablespace file.
@param[in]	space_id	Tablespace ID
@param[in]	name		Tablespace name in dbname/tablename format.
@param[in]	path		Path and filename of the datafile to create.
@param[in]	flags		Tablespace flags
@param[in]	size		Initial size of the tablespace file in pages,
must be >= FIL_IBD_FILE_INITIAL_SIZE
@param[in]	mode		MariaDB encryption mode
@param[in]	key_id		MariaDB encryption key_id
@param[out]	err		DB_SUCCESS or error code
@return	the created tablespace
@retval	NULL	on error */
fil_space_t*
fil_ibd_create(
	uint32_t	space_id,
	const table_name_t name,
	const char*	path,
	uint32_t	flags,
	uint32_t	size,
	fil_encryption_t mode,
	uint32_t	key_id,
	dberr_t*	err)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Try to adjust FSP_SPACE_FLAGS if they differ from the expectations.
(Typically when upgrading from MariaDB 10.1.0..10.1.20.)
@param[in,out]	space		tablespace
@param[in]	flags		desired tablespace flags */
void fsp_flags_try_adjust(fil_space_t *space, uint32_t flags);

/********************************************************************//**
Tries to open a single-table tablespace and optionally checks the space id is
right in it. If does not succeed, prints an error message to the .err log. This
function is used to open a tablespace when we start up mysqld, and also in
IMPORT TABLESPACE.
NOTE that we assume this operation is used either at the database startup
or under the protection of dict_sys.latch, so that two users cannot
race here. This operation does not leave the file associated with the
tablespace open, but closes it after we have looked at the space id in it.

If the validate boolean is set, we read the first page of the file and
check that the space id in the file is what we expect. We assume that
this function runs much faster if no check is made, since accessing the
file inode probably is much faster (the OS caches them) than accessing
the first page of the file.  This boolean may be initially false, but if
a remote tablespace is found it will be changed to true.

@param[in]	validate	0=maybe missing, 1=do not validate, 2=validate
@param[in]	purpose		FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in]	id		tablespace ID
@param[in]	flags		expected FSP_SPACE_FLAGS
@param[in]	name		table name
If file-per-table, it is the table name in the databasename/tablename format
@param[in]	path_in		expected filepath, usually read from dictionary
@param[out]	err		DB_SUCCESS or error code
@return	tablespace
@retval	NULL	if the tablespace could not be opened */
fil_space_t*
fil_ibd_open(
	unsigned		validate,
	fil_type_t		purpose,
	uint32_t		id,
	uint32_t		flags,
	fil_space_t::name_type	name,
	const char*		path_in,
	dberr_t*		err = NULL)
	MY_ATTRIBUTE((warn_unused_result));

enum fil_load_status {
	/** The tablespace file(s) were found and valid. */
	FIL_LOAD_OK,
	/** The name no longer matches space_id */
	FIL_LOAD_ID_CHANGED,
	/** The file(s) were not found */
	FIL_LOAD_NOT_FOUND,
	/** The file(s) were not valid */
	FIL_LOAD_INVALID,
	/** The tablespace file was deferred to open */
	FIL_LOAD_DEFER
};

/** Open a single-file tablespace and add it to the InnoDB data structures.
@param[in]	space_id	tablespace ID
@param[in]	filename	path/to/databasename/tablename.ibd
@param[out]	space		the tablespace, or NULL on error
@return status of the operation */
enum fil_load_status
fil_ibd_load(uint32_t space_id, const char *filename, fil_space_t *&space)
  MY_ATTRIBUTE((warn_unused_result));

/** Determine if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id		Tablespace ID
@param[in]	table_flags	table flags
@return the tablespace
@retval	NULL	if no matching tablespace exists in the memory cache */
fil_space_t *fil_space_for_table_exists_in_mem(uint32_t id,
                                               uint32_t table_flags);

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool fil_space_extend(fil_space_t *space, uint32_t size);

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS. */
void fil_flush_file_spaces();
/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */
bool fil_validate();
/*********************************************************************//**
Sets the file page type. */
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type);	/*!< in: type */

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */
void
fil_delete_file(
/*============*/
	const char*	path);	/*!< in: filepath of the ibd tablespace */

/** Look up a tablespace.
@param tablespace identifier
@return tablespace
@retval nullptr if not found */
fil_space_t *fil_space_get_by_id(uint32_t id);

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space);


bool fil_comp_algo_loaded(ulint comp_algo);

/** On a log checkpoint, reset fil_names_dirty_and_write() flags
and write out FILE_MODIFY if needed, and write FILE_CHECKPOINT.
@param lsn  checkpoint LSN
@return current LSN */
lsn_t fil_names_clear(lsn_t lsn);

#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
void test_make_filepath();
#endif /* UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */

/** Determine the block size of the data file.
@param[in]	space		tablespace
@param[in]	offset		page number
@return	block size */
ulint fil_space_get_block_size(const fil_space_t* space, unsigned offset);

#include "fil0fil.inl"
#endif /* UNIV_INNOCHECKSUM */

#endif /* fil0fil_h */
