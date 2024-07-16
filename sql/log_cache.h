/*
   Copyright (c) 2023, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "log_event.h"
#include "mysql/service_wsrep.h"

static constexpr my_off_t MY_OFF_T_UNDEF= ~0ULL;
/** Truncate cache log files bigger than this */
static constexpr my_off_t CACHE_FILE_TRUNC_SIZE = 65536;


/*
  Helper classes to store non-transactional and transactional data
  before copying it to the binary log.
*/

class binlog_cache_data
{
public:
  binlog_cache_data(bool precompute_checksums):
                    before_stmt_pos(MY_OFF_T_UNDEF), m_pending(0), status(0),
                    incident(FALSE), precompute_checksums(precompute_checksums),
                    saved_max_binlog_cache_size(0), ptr_binlog_cache_use(0),
                    ptr_binlog_cache_disk_use(0), m_file_reserved_bytes(0)
  {
    /*
      Read the current checksum setting. We will use this setting to decide
      whether to pre-compute checksums in the cache. Then when writing the cache
      to the actual binlog, another check will be made and checksums recomputed
      in the unlikely case that the setting changed meanwhile.
    */
    checksum_opt= !precompute_checksums ? BINLOG_CHECKSUM_ALG_OFF :
      (enum_binlog_checksum_alg)binlog_checksum_options;
  }

  virtual ~binlog_cache_data()
  {
    DBUG_ASSERT(empty());

    if (m_file_reserved_bytes > 0 && cache_log.file != -1)
      unlink(my_filename(cache_log.file));

    close_cached_file(&cache_log);
  }

  /*
    Return 1 if there is no relevant entries in the cache

    This is:
    - Cache is empty
    - There are row or critical (DDL?) events in the cache

    The status test is needed to avoid writing entries with only
    a table map entry, which would crash in do_apply_event() on the slave
    as it assumes that there is always a row entry after a table map.
  */
  bool empty() const
  {
    return (pending() == NULL &&
            (my_b_write_tell(&cache_log) - m_file_reserved_bytes == 0 ||
             ((status & (LOGGED_ROW_EVENT | LOGGED_CRITICAL)) == 0)));
  }

  Rows_log_event *pending() const
  {
    return m_pending;
  }

  void set_pending(Rows_log_event *const pending_arg)
  {
    m_pending= pending_arg;
  }

  void set_incident(void)
  {
    incident= TRUE;
  }

  bool has_incident(void) const
  {
    return(incident);
  }

  void reset()
  {
    bool cache_was_empty= empty();
    bool truncate_file= (cache_log.file != -1 &&
                         my_b_write_tell(&cache_log) >
                         MY_MIN(CACHE_FILE_TRUNC_SIZE, binlog_stmt_cache_size));
    truncate(0, 1); // Forget what's in cache
    checksum_opt= !precompute_checksums ? BINLOG_CHECKSUM_ALG_OFF :
      (enum_binlog_checksum_alg)binlog_checksum_options;
    if (!cache_was_empty)
      compute_statistics();
    if (truncate_file)
      truncate_io_cache(&cache_log);
    status= 0;
    incident= FALSE;
    before_stmt_pos= MY_OFF_T_UNDEF;
    DBUG_ASSERT(empty());
  }

  my_off_t get_byte_position() const
  {
    DBUG_ASSERT(cache_log.type == WRITE_CACHE);
    return my_b_tell(&cache_log) - m_file_reserved_bytes;
  }

  my_off_t get_prev_position() const
  {
    return(before_stmt_pos);
  }

  void set_prev_position(my_off_t pos)
  {
    before_stmt_pos= pos;
  }

  void restore_prev_position()
  {
    truncate(before_stmt_pos);
  }

  void restore_savepoint(my_off_t pos)
  {
    truncate(pos);
    if (pos < before_stmt_pos)
      before_stmt_pos= MY_OFF_T_UNDEF;
  }

  void set_binlog_cache_info(my_off_t param_max_binlog_cache_size,
                             ulong *param_ptr_binlog_cache_use,
                             ulong *param_ptr_binlog_cache_disk_use)
  {
    /*
      The assertions guarantee that the set_binlog_cache_info is
      called just once and information passed as parameters are
      never zero.

      This is done while calling the constructor binlog_cache_mngr.
      We cannot set information in the constructor binlog_cache_data
      because the space for binlog_cache_mngr is allocated through
      a placement new.

      In the future, we can refactor this and change it to avoid
      the set_binlog_info.
    */
    DBUG_ASSERT(saved_max_binlog_cache_size == 0);
    DBUG_ASSERT(param_max_binlog_cache_size != 0);
    DBUG_ASSERT(ptr_binlog_cache_use == 0);
    DBUG_ASSERT(param_ptr_binlog_cache_use != 0);
    DBUG_ASSERT(ptr_binlog_cache_disk_use == 0);
    DBUG_ASSERT(param_ptr_binlog_cache_disk_use != 0);

    saved_max_binlog_cache_size= param_max_binlog_cache_size;
    ptr_binlog_cache_use= param_ptr_binlog_cache_use;
    ptr_binlog_cache_disk_use= param_ptr_binlog_cache_disk_use;
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  void add_status(enum_logged_status status_arg)
  {
    status|= status_arg;
  }

  bool write_check(size_t write_length)
  {
    if (unlikely(get_byte_position() == 0 && support_reserve() &&
                 !encrypt_tmp_files))
      init_file_reserved_bytes();
    if (unlikely(m_file_reserved_bytes > 0 &&
                 cache_log.write_pos + write_length > cache_log.write_end &&
                 cache_log.file == -1))
      return init_tmp_file();
    return false;
  }

  bool init_for_read()
  {
    return reinit_io_cache(&cache_log, READ_CACHE, m_file_reserved_bytes, 0, 0);
  }

  my_off_t length_for_read() const
  {
    DBUG_ASSERT(cache_log.type == READ_CACHE);
    return cache_log.end_of_file - m_file_reserved_bytes;
  }

  my_off_t temp_file_length()
  {
    return my_b_tell(&cache_log);
  }

  uint32 file_reserved_bytes() { return m_file_reserved_bytes; }

  bool sync_tmp_file()
  {
    DBUG_ASSERT(cache_log.file != -1);

    if (my_b_flush_io_cache(&cache_log, 1) ||
        mysql_file_sync(cache_log.file, MYF(MY_WME)))
      return true;
    return false;
  }

  void tmp_file_name(char name[FN_REFLEN]);

  void detach_temp_file();

  /*
    Cache to store data before copying it to the binary log.
  */
  IO_CACHE cache_log;

protected:
  /*
    Binlog position before the start of the current statement.
  */
  my_off_t before_stmt_pos;

private:
  /*
    Pending binrows event. This event is the event where the rows are currently
    written.
   */
  Rows_log_event *m_pending;

  /*
    Bit flags for what has been writing to cache. Used to
    discard logs without any data changes.
    see enum_logged_status;
  */
  uint32 status;

public:
  /*
    The algorithm (if any) used to pre-compute checksums in the cache.
    Initialized from binlog_checksum_options when the cache is reset.
  */
  enum_binlog_checksum_alg checksum_opt;

private:
  /*
    This indicates that some events did not get into the cache and most likely
    it is corrupted.
  */
  bool incident;

  /* Whether the caller requested precomputing checksums. */
  bool precompute_checksums;

  /**
    This function computes binlog cache and disk usage.
  */
  void compute_statistics()
  {
    statistic_increment(*ptr_binlog_cache_use, &LOCK_status);
    if (cache_log.disk_writes != 0)
    {
#ifdef REAL_STATISTICS
      statistic_add(*ptr_binlog_cache_disk_use,
                    cache_log.disk_writes, &LOCK_status);
#else
      statistic_increment(*ptr_binlog_cache_disk_use, &LOCK_status);
#endif
      cache_log.disk_writes= 0;
    }
  }

  /*
    Stores the values of maximum size of the cache allowed when this cache
    is configured. This corresponds to either
      . max_binlog_cache_size or max_binlog_stmt_cache_size.
  */
  my_off_t saved_max_binlog_cache_size;

  /*
    Stores a pointer to the status variable that keeps track of the in-memory
    cache usage. This corresponds to either
      . binlog_cache_use or binlog_stmt_cache_use.
  */
  ulong *ptr_binlog_cache_use;

  /*
    Stores a pointer to the status variable that keeps track of the disk
    cache usage. This corresponds to either
      . binlog_cache_disk_use or binlog_stmt_cache_disk_use.
  */
  ulong *ptr_binlog_cache_disk_use;

  uint32 m_file_reserved_bytes;

  /*
    It truncates the cache to a certain position. This includes deleting the
    pending event.
   */
  void truncate(my_off_t pos, bool reset_cache=0)
  {
    DBUG_PRINT("info", ("truncating to position %lu", (ulong) pos));
    cache_log.error=0;
    if (pending())
    {
      delete pending();
      set_pending(0);
    }
    my_bool res __attribute__((unused))= reinit_io_cache(
        &cache_log, WRITE_CACHE, pos + m_file_reserved_bytes, 0, reset_cache);
    DBUG_ASSERT(res == 0);
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  void init_file_reserved_bytes();
  bool init_tmp_file();

  virtual bool support_reserve() { return false; }

  binlog_cache_data& operator=(const binlog_cache_data& info);
  binlog_cache_data(const binlog_cache_data& info);
};

class Reserve_space_binlog_cache_data : public binlog_cache_data
{
public:
  Reserve_space_binlog_cache_data(bool precompute_checksums)
      : binlog_cache_data(precompute_checksums)
  {
  }

private:
  bool support_reserve() override { return true; }
};

/**
  This class provide functions of Binlog cache free flush, the main process is
  in ::commit().
*/
class Binlog_cache_manager {
public:
  Binlog_cache_manager() {}

  /**
    Create or clear the binlog cache dir to store the binlog cache temp file, at
    the same dir with binlog file.

    Binlog cache free flush change the temp file type from UNLINK_FILE to
    KEEP_FILE, because UNLINK_FILE cannot be persisted to file system before
    Linux 3.11.

    Use the dir created by this function to contain binlog cache temp file.
    Binlog cache will clear its temp file during destruction. If server not
    shutdown normally, this dir will be cleared during next time startup.
  */
  void init_binlog_cache_dir();

  bool dir_created() { return m_dir_created; }
private:
  /** Whether binlog cache dir created successfully. */
  bool m_dir_created { false };
};
extern Binlog_cache_manager binlog_cache_manager;
