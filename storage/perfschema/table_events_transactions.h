/* Copyright (c) 2010, 2023, Oracle and/or its affiliates.

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
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef TABLE_EVENTS_TRANSACTIONS_H
#define TABLE_EVENTS_TRANSACTIONS_H

/**
  @file storage/perfschema/table_events_HA_ERR_WRONG_COMMAND.h
  Table EVENTS_TRANSACTIONS_xxx (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "pfs_events_transactions.h"
#include "table_helper.h"
#include "rpl_gtid.h"

struct PFS_thread;

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** A row of table_events_transactions_common. */
struct row_events_transactions
{
  /** Column THREAD_ID. */
  ulonglong m_thread_internal_id;
  /** Column EVENT_ID. */
  ulonglong m_event_id;
  /** Column END_EVENT_ID. */
  ulonglong m_end_event_id;
  /** Column NESTING_EVENT_ID. */
  ulonglong m_nesting_event_id;
  /** Column NESTING_EVENT_TYPE. */
  enum_event_type m_nesting_event_type;
  /** Column EVENT_NAME. */
  const char *m_name;
  /** Length in bytes of @c m_name. */
  uint m_name_length;
  /** Column TIMER_START. */
  ulonglong m_timer_start;
  /** Column TIMER_END. */
  ulonglong m_timer_end;
  /** Column TIMER_WAIT. */
  ulonglong m_timer_wait;
  /** Column SOURCE. */
  char m_source[COL_SOURCE_SIZE];
  /** Length in bytes of @c m_source. */
  uint m_source_length;
  /** InnoDB transaction id. */
  ulonglong m_trxid;
  /** Transaction state. */
  enum_transaction_state m_state;
  /** Global Transaction ID. */
  char m_gtid[GTID_MAX_STR_LENGTH + 1];
  /** GTID length in bytes*/
  int m_gtid_length;
  /** XA transaction ID. */
  PSI_xid m_xid;
  /** XA transaction state. */
  enum_xa_transaction_state m_xa_state;
  /** True if XA transaction. */
  bool m_xa;
  /** True if autocommit transaction. */
  bool m_autocommit;
  /** Isolation level. */
  enum_isolation_level m_isolation_level;
  /** True if read-only, read-write otherwise. */
  bool m_read_only;
  /** Column NUMBER_OF_SAVEPOINTS. */
  ulonglong m_savepoint_count;
  /** Column NUMBER_OF_ROLLBACK_TO_SAVEPOINT. */
  ulonglong m_rollback_to_savepoint_count;
  /** Column NUMBER_OF_RELEASE_SAVEPOINT. */
  ulonglong m_release_savepoint_count;
};

/**
  Position of a cursor on PERFORMANCE_SCHEMA.EVENTS_TRANSACTIONS_HISTORY.
  Index 1 on thread (0 based)
  Index 2 on transaction event record in thread history (0 based)
*/
struct pos_events_transactions_history : public PFS_double_index
{
  pos_events_transactions_history()
    : PFS_double_index(0, 0)
  {}

  inline void reset(void)
  {
    m_index_1= 0;
    m_index_2= 0;
  }

  inline void next_thread(void)
  {
    m_index_1++;
    m_index_2= 0;
  }
};

/**
  Adapter, for table sharing the structure of
  PERFORMANCE_SCHEMA.EVENTS_TRANSACTIONS_CURRENT.
*/
class table_events_transactions_common : public PFS_engine_table
{
protected:
  int read_row_values(TABLE *table,
                      unsigned char *buf,
                      Field **fields,
                      bool read_all) override;

  table_events_transactions_common(const PFS_engine_table_share *share, void *pos);

  ~table_events_transactions_common()
  {}

  void make_row(PFS_events_transactions *statement);

  /** Current row. */
  row_events_transactions m_row;
  /** True if the current row exists. */
  bool m_row_exists;
};

/** Table PERFORMANCE_SCHEMA.EVENTS_TRANSACTIONS_CURRENT. */
class table_events_transactions_current : public table_events_transactions_common
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static int delete_all_rows();
  static ha_rows get_row_count();

  int rnd_init(bool scan) override;
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

protected:
  table_events_transactions_current();

public:
  ~table_events_transactions_current()
  {}

private:
  friend class table_events_transactions_history;
  friend class table_events_transactions_history_long;

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /**
    Fields definition.
    Also used by table_events_transactions_history
    and table_events_transactions_history_long.
  */
  static TABLE_FIELD_DEF m_field_def;

  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;
};

/** Table PERFORMANCE_SCHEMA.EVENTS_TRANSACTIONS_HISTORY. */
class table_events_transactions_history : public table_events_transactions_common
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static int delete_all_rows();
  static ha_rows get_row_count();

  int rnd_init(bool scan) override;
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

protected:
  table_events_transactions_history();

public:
  ~table_events_transactions_history()
  {}

private:
  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current position. */
  pos_events_transactions_history m_pos;
  /** Next position. */
  pos_events_transactions_history m_next_pos;
};

/** Table PERFORMANCE_SCHEMA.EVENTS_TRANSACTIONS_HISTORY_LONG. */
class table_events_transactions_history_long : public table_events_transactions_common
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static int delete_all_rows();
  static ha_rows get_row_count();

  int rnd_init(bool scan) override;
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

protected:
  table_events_transactions_history_long();

public:
  ~table_events_transactions_history_long()
  {}

private:
  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;
};

/** @} */
#endif
