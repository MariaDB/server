/*
  Copyright (c) 2004, 2010, Oracle and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/** @file ha_example.h

    @brief
  The ha_example engine is a stubbed storage engine for example purposes only;
  it does nothing at this point. Its purpose is to provide a source
  code illustration of how to begin writing new storage engines; see also
  /storage/example/ha_example.cc.

    @note
  Please read ha_example.cc before reading this file.
  Reminder: The example storage engine implements all methods that are *required*
  to be implemented. For a full list of all methods that you can implement, see
  handler.h.

   @see
  /sql/handler.h and /storage/example/ha_example.cc
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "my_global.h"                   /* ulonglong */
#include "thr_lock.h"                    /* THR_LOCK, THR_LOCK_DATA */
#include "handler.h"                     /* handler */
#include "my_base.h"                     /* ha_rows */

/** @brief
  Example_share is a class that will be shared among all open handlers.
  This example implements the minimum of what you will probably need.
*/
class Example_share : public Handler_share {
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  Example_share();
  ~Example_share()
  {
    thr_lock_delete(&lock);
    mysql_mutex_destroy(&mutex);
  }
};

/** @brief
  Class definition for the storage engine
*/
class ha_example: public handler
{
  THR_LOCK_DATA lock;      ///< MariaDB lock
  Example_share *share;    ///< Shared lock info
  Example_share *get_share(); ///< Get the share

public:
  ha_example(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_example() = default;

  /** @brief
    The name of the index type that will be used for display.
    Don't implement this method unless you really have indexes.
   */
  const char *index_type(uint inx) override { return "HASH"; }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override
  {
    /*
      We are saying that this engine is just statement capable to have
      an engine that can only handle statement-based logging. This is
      used in testing.
    */
    return HA_BINLOG_STMT_CAPABLE;
  }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MariaDB wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return 0;
  }

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MariaDB will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MariaDB will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_keys() const override { return 0; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MariaDB will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_parts() const override { return 0; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MariaDB will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length() const override { return 0; }

  /** @brief
    Called in test_quick_select to determine cost of table scan
  */
  virtual IO_AND_CPU_COST scan_time() override
  {
    IO_AND_CPU_COST cost;
    /* 0 blocks,  0.001 ms / row */
    cost.io= (double) (stats.records+stats.deleted) * DISK_READ_COST;
    cost.cpu= 0;
    return cost;
  }

  /** @brief
    This method will never be called if you do not implement indexes.
  */
  IO_AND_CPU_COST keyread_time(uint, ulong, ha_rows rows,
                                       ulonglong blocks) override
  {
    IO_AND_CPU_COST cost;
    cost.io= blocks * DISK_READ_COST;
    cost.cpu= (double) rows * 0.001;
    return cost;
  }

  /** @brief
    Cost of fetching 'rows' records through rnd_pos()
  */
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override
  {
   IO_AND_CPU_COST cost;
    /* 0 blocks,  0.001 ms / row */
    cost.io= 0;
    cost.cpu= (double) rows * DISK_READ_COST;
    return cost;
  }

  /*
    Everything below are methods that we implement in ha_example.cc.

    Most of these methods are not obligatory, skip them and
    MariaDB will treat them as not implemented
  */
  /** @brief
    We implement this in ha_example.cc; it's a required method.
  */
  int open(const char *name, int mode, uint test_if_locked) override;    // required

  /** @brief
    We implement this in ha_example.cc; it's a required method.
  */
  int close(void) override;                                              // required

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int write_row(const uchar *buf) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int update_row(const uchar *old_data, const uchar *new_data) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int delete_row(const uchar *buf) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int index_read_map(uchar *buf, const uchar *key,
                     key_part_map keypart_map, enum ha_rkey_function find_flag) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int index_next(uchar *buf) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int index_prev(uchar *buf) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int index_first(uchar *buf) override;

  /** @brief
    We implement this in ha_example.cc. It's not an obligatory method;
    skip it and and MariaDB will treat it as not implemented.
  */
  int index_last(uchar *buf) override;

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan) override;                                      //required
  int rnd_end() override;
  int rnd_next(uchar *buf) override;                                     ///< required
  int rnd_pos(uchar *buf, uchar *pos) override;                          ///< required
  void position(const uchar *record) override;                           ///< required
  int info(uint) override;                                               ///< required
  int extra(enum ha_extra_function operation) override;
  int external_lock(THD *thd, int lock_type) override;                   ///< required
  int delete_all_rows(void) override;
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *pages) override;
  int delete_table(const char *from) override;
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;                      ///< required
  enum_alter_inplace_result
  check_if_supported_inplace_alter(TABLE* altered_table,
                                   Alter_inplace_info* ha_alter_info) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;     ///< required
};
