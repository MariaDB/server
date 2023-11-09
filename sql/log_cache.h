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


class binlog_cache_data
{
public:
  binlog_cache_data(): before_stmt_pos(MY_OFF_T_UNDEF), m_pending(0), status(0),
                       incident(FALSE), saved_max_binlog_cache_size(0), ptr_binlog_cache_use(0),
                       ptr_binlog_cache_disk_use(0)
  { }

  ~binlog_cache_data()
  {
    DBUG_ASSERT(empty());
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
            (my_b_write_tell(&cache_log) == 0 ||
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
                         my_b_write_tell(&cache_log) > CACHE_FILE_TRUNC_SIZE);
    truncate(0,1);                              // Forget what's in cache
    if (!cache_was_empty)
      compute_statistics();
    if (truncate_file)
      my_chsize(cache_log.file, 0, 0, MYF(MY_WME));

    status= 0;
    incident= FALSE;
    before_stmt_pos= MY_OFF_T_UNDEF;
    DBUG_ASSERT(empty());
  }

  my_off_t get_byte_position() const
  {
    return my_b_tell(&cache_log);
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

  /*
    This indicates that some events did not get into the cache and most likely
    it is corrupted.
  */
  bool incident;

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
    my_bool res= reinit_io_cache(&cache_log, WRITE_CACHE, pos, 0, reset_cache);
    DBUG_ASSERT(res == 0);
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  binlog_cache_data& operator=(const binlog_cache_data& info);
  binlog_cache_data(const binlog_cache_data& info);
};
