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

static constexpr my_off_t MY_OFF_T_UNDEF= ~0ULL;
/** Truncate cache log files bigger than this */
static constexpr my_off_t CACHE_FILE_TRUNC_SIZE = 65536;

/**
  Create binlog cache directory if it doesn't exist, otherwise delete all
  files existing in the directory.

  @retval false   Succeeds to initialize the directory.
  @retval true    Failed to initialize the directory.
*/
bool init_binlog_cache_dir();

extern char binlog_cache_dir[FN_REFLEN];

/*
  Helper classes to store non-transactional and transactional data
  before copying it to the binary log.
*/

class binlog_cache_data
{
public:
  binlog_cache_data(bool trx_cache, bool precompute_checksums):
                    engine_binlog_info {0, 0, 0, 0, 0, 0, 0, 0},
                    before_stmt_pos(MY_OFF_T_UNDEF), m_pending(0), status(0),
                    is_trx_cache(trx_cache),
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

  ~binlog_cache_data()
  {
    DBUG_ASSERT(empty());

    if (cache_log.file != -1 && !encrypt_tmp_files)
      unlink(my_filename(cache_log.file));

    if (engine_binlog_info.engine_ptr)
      (*opt_binlog_engine_hton->binlog_oob_free)
        (engine_binlog_info.engine_ptr);
    close_cached_file(&cache_log);
  }

  bool trx_cache() { return is_trx_cache; }

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

  void clear_incident(void)
  {
    incident= FALSE;
  }

  bool has_incident(void) const
  {
    return(incident);
  }

  void reset()
  {
    bool cache_was_empty= empty();
    my_off_t trunc_len= MY_MIN(CACHE_FILE_TRUNC_SIZE, cache_log.buffer_length);
    bool truncate_file= (cache_log.file != -1 &&
                         my_b_write_tell(&cache_log) > trunc_len);
    // m_file_reserved_bytes must be reset to 0, before truncate.
    m_file_reserved_bytes= 0;
    truncate(0,1);                              // Forget what's in cache
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

  void truncate_cache_file()
  {
    DBUG_ASSERT(empty());
    if (cache_log.file != -1)
      truncate_io_cache(&cache_log);
  }

  void reset_for_engine_binlog()
  {
    bool cache_was_empty= empty();

    if (engine_binlog_info.engine_ptr)
      (*opt_binlog_engine_hton->binlog_oob_reset)
        (&engine_binlog_info.engine_ptr);
    engine_binlog_info.engine_ptr2= nullptr;
    engine_binlog_info.xa_xid= nullptr;
    engine_binlog_info.out_of_band_offset= 0;
    engine_binlog_info.gtid_offset= 0;
    engine_binlog_info.internal_xa= false;
    /* Preserve the engine_ptr for the engine to re-use, was reset above. */

    truncate(cache_log.pos_in_file);
    cache_log.pos_in_file= 0;
    cache_log.request_pos= cache_log.write_pos= cache_log.buffer;
    cache_log.write_end= cache_log.buffer + cache_log.buffer_length;
    checksum_opt= BINLOG_CHECKSUM_ALG_OFF;
    if (!cache_was_empty)
      compute_statistics();
    status= 0;
    incident= FALSE;
    before_stmt_pos= MY_OFF_T_UNDEF;
    DBUG_ASSERT(empty());
  }

  void reset_cache_for_engine(my_off_t pos,
         int (*fct)(struct st_io_cache *, const uchar *, size_t))
  {
    /* Bit fiddly here as we're abusing the IO_CACHE a bit for oob handling. */
    cache_log.pos_in_file= pos;
    cache_log.request_pos= cache_log.write_pos= cache_log.buffer;
    cache_log.write_end=
      (cache_log.buffer + cache_log.buffer_length - (pos & (IO_SIZE-1)));
    cache_log.write_function= fct;
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

  /**
    This function is called everytime when anything is being written into the
    cache_log. To support rename binlog cache to binlog file, the cache_log
    should be initialized with reserved space.
  */
  bool write_prepare(size_t write_length)
  {
    /* Data will exceed the buffer size in this write */
    if (unlikely(cache_log.write_pos + write_length > cache_log.write_end &&
                 cache_log.pos_in_file == 0))
    {
      /* Only session's binlog cache need to reserve space. */
      if (cache_log.dir == binlog_cache_dir && !encrypt_tmp_files)
        return init_file_reserved_bytes();
    }
    return false;
  }

  /**
    For session's binlog cache, it have to call this function to skip the
    reserved before reading the cache file.
  */
  bool init_for_read()
  {
    return reinit_io_cache(&cache_log, READ_CACHE, m_file_reserved_bytes, 0, 0);
  }

  /**
    For session's binlog cache, it have to call this function to get the
    actual data length.
  */
  my_off_t length_for_read() const
  {
    DBUG_ASSERT(cache_log.type == READ_CACHE);
    return cache_log.end_of_file - m_file_reserved_bytes;
  }

  /**
    It function returns the cache file's actual length which includes the
    reserved space.
   */
  my_off_t temp_file_length()
  {
    return my_b_tell(&cache_log);
  }

  uint32 file_reserved_bytes() { return m_file_reserved_bytes; }

  /**
    Flush and sync the data of the file into storage.

    @retval true    Error happens
    @retval false   Succeeds
  */
  bool sync_temp_file()
  {
    DBUG_ASSERT(cache_log.file != -1);

    if (my_b_flush_io_cache(&cache_log, 1) ||
        mysql_file_sync(cache_log.file, MYF(0)))
      return true;
    return false;
  }

  /**
    Copy the name of the cache file to the argument name.
  */
  const char *temp_file_name() { return my_filename(cache_log.file); }

  /**
    It is called after renaming the cache file to a binlog file. The file
    now is a binlog file, so detach it from the binlog cache.
  */
  void detach_temp_file();

  /*
    Cache to store data before copying it to the binary log.
  */
  IO_CACHE cache_log;
  /* Context for engine-implemented binlogging. */
  handler_binlog_event_group_info engine_binlog_info;

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
  /* Record whether this is a stmt or trx cache. */
  bool is_trx_cache;
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

  /*
    Stores the bytes reserved at the begin of the cache file. It could be
    0 for cases that reserved space are not supported. see write_prepare().
  */
  uint32 m_file_reserved_bytes {0};

  /*
    It truncates the cache to a certain position. This includes deleting the
    pending event. Note that file size does not change.
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

  /**
    Reserve required space at the begin of the tempoary file. It will create
    the temporary file if it doesn't exist.
  */
  bool init_file_reserved_bytes();

  binlog_cache_data& operator=(const binlog_cache_data& info);
  binlog_cache_data(const binlog_cache_data& info);
};
