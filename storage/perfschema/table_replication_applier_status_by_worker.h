/*
   Copyright (c) 2013, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#ifndef TABLE_REPLICATION_APPLIER_STATUS_BY_WORKER_H
#define TABLE_REPLICATION_APPLIER_STATUS_BY_WORKER_H

/**
  @file storage/perfschema/table_replication_applier_status_by_worker.h
  Table replication_applier_status_by_worker (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "rpl_mi.h"
#include "mysql_com.h"
#include "my_thread.h"
#include "rpl_parallel.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

#ifndef ENUM_RPL_YES_NO
#define ENUM_RPL_YES_NO
/** enumerated values for service_state of worker thread*/
enum enum_rpl_yes_no {
  PS_RPL_YES= 1, /* service_state= on */
  PS_RPL_NO /* service_state= off */
};
#endif

/*
  A row in worker's table. The fields with string values have an additional
  length field denoted by <field_name>_length.
*/
struct st_row_worker {

  char channel_name[CHANNEL_NAME_LENGTH];
  uint channel_name_length;
  ulonglong thread_id;
  uint thread_id_is_null;
  enum_rpl_yes_no service_state;
  char last_seen_transaction[GTID_MAX_STR_LENGTH + 1];
  uint last_seen_transaction_length;
  uint last_error_number;
  char last_error_message[MAX_SLAVE_ERRMSG];
  uint last_error_message_length;
  ulonglong last_error_timestamp;
  ulonglong worker_idle_time;
  ulong last_trans_retry_count;
};

/**
  Position in table replication_applier_status_by_worker.
  We have global replication thread pool.
*/
struct pos_replication_applier_status_by_worker : public PFS_simple_index
{

  pos_replication_applier_status_by_worker() : PFS_simple_index(0)
  {}

  inline void reset(void)
  {
    m_index= 0;
  }

  inline bool has_more_workers(uint num)
  { return (m_index < num); }

  inline void next_worker(void)
  {
    m_index++;
  }
};


/** Table PERFORMANCE_SCHEMA.replication_applier_status_by_worker */
class table_replication_applier_status_by_worker: public PFS_engine_table
{
  typedef pos_replication_applier_status_by_worker pos_t;

private:
  /*
    Master_info to construct a row to display SQL Thread's status
    information in STS mode
  */
  void make_row(rpl_parallel_thread *);
  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Fields definition. */
  static TABLE_FIELD_DEF m_field_def;
  /** current row*/
  st_row_worker m_row;
  /** True is the current row exists. */
  bool m_row_exists;
  /** Current position. */
  pos_t m_pos;
  /** Next position. */
  pos_t m_next_pos;

protected:
  /**
    Read the current row values.
    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */

  virtual int read_row_values(TABLE *table,
                              unsigned char *buf,
                              Field **fields,
                              bool read_all);

  table_replication_applier_status_by_worker();

public:
  ~table_replication_applier_status_by_worker();

  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static ha_rows get_row_count();
  virtual int rnd_next();
  virtual int rnd_pos(const void *pos);
  virtual void reset_position(void);

};

/** @} */
#endif
