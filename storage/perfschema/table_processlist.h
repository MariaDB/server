/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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

#ifndef TABLE_PROCESSLIST_H
#define TABLE_PROCESSLIST_H

/**
  @file storage/perfschema/table_processlist.h
  TABLE THREADS.
*/

#include <sys/types.h>
#include <time.h>
#include <algorithm>

// #include "my_hostname.h"
// #include "my_inttypes.h"
#include "cursor_by_thread.h"
#include "pfs_column_types.h"

struct PFS_thread;

/**
  @addtogroup performance_schema_tables
  @{
*/

/**
  A row of PERFORMANCE_SCHEMA.PROCESSLIST.
*/
struct row_processlist {
  /** Column ID. */
  ulonglong m_processlist_id;
  /** Column USER. */
  char m_username[USERNAME_LENGTH];
  /** Length in bytes of @c m_username. */
  uint m_username_length;
  /** Column HOST. */
  char m_hostname[HOST_AND_PORT_LENGTH];
  /** Length in bytes of @c m_hostname. */
  uint m_hostname_length;
  /** Port. */
  uint m_port;
  /** Column DB. */
  char m_dbname[NAME_LEN];
  /** Length in bytes of @c m_dbname. */
  uint m_dbname_length;
  /** Column COMMAND. */
  int m_command;
  /** Column TIME. */
  time_t m_start_time;
  /** Column STATE. */
  const char *m_processlist_state_ptr;
  /** Length in bytes of @c m_processlist_state_ptr. */
  uint m_processlist_state_length;
  /** Column INFO. */
  const char *m_processlist_info_ptr;
  /** Length in bytes of @c m_processlist_info_ptr. */
  uint m_processlist_info_length;
};

enum enum_priv_processlist {
  /** User is not allowed to see any data. */
  PROCESSLIST_DENIED,
  /** User does not have the PROCESS_ACL priviledge. */
  PROCESSLIST_USER_ONLY,
  /** User has the PROCESS_ACL priviledge. */
  PROCESSLIST_ALL
};

struct row_priv_processlist {
  enum enum_priv_processlist m_auth;
  char m_priv_user[USERNAME_LENGTH];
  size_t m_priv_user_length;
};

/** Table PERFORMANCE_SCHEMA.PROCESSLIST. */
class table_processlist : public cursor_by_thread {
 public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  /** Table builder */
  static PFS_engine_table *create();

 protected:
  table_processlist();

  virtual int rnd_init(bool scan);

  virtual int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all);
  int set_access(void);

 public:
  virtual ~table_processlist() {}

 private:
  virtual void make_row(PFS_thread *pfs);
  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Fields definition. */
  static TABLE_FIELD_DEF m_field_def;

  /** Current row. */
  row_processlist m_row;
  /** Row privileges. */
  row_priv_processlist m_row_priv;
  /** True if the current row exists. */
  bool m_row_exists;
};

/** @} */
#endif
