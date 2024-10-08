/* Copyright (c) 2012, 2023, Oracle and/or its affiliates.

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
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef TABLE_SESSION_CONNECT_H
#define TABLE_SESSION_CONNECT_H

#include "pfs_column_types.h"
#include "cursor_by_thread_connect_attr.h"
#include "table_helper.h"

#define MAX_ATTR_NAME_CHARS 32
#define MAX_ATTR_VALUE_CHARS 1024
#define MAX_UTF8_BYTES 6

/** symbolic names for field offsets, keep in sync with field_types */
enum field_offsets {
  FO_PROCESS_ID,
  FO_ATTR_NAME,
  FO_ATTR_VALUE,
  FO_ORDINAL_POSITION
};

/**
  A row of PERFORMANCE_SCHEMA.SESSION_CONNECT_ATTRS and
  PERFORMANCE_SCHEMA.SESSION_ACCOUNT_CONNECT_ATTRS.
*/
struct row_session_connect_attrs
{
  /** Column PROCESS_ID. */
  ulong m_process_id;
  /** Column ATTR_NAME. In UTF-8 */
  char m_attr_name[MAX_ATTR_NAME_CHARS * MAX_UTF8_BYTES];
  /** Length in bytes of @c m_attr_name. */
  uint m_attr_name_length;
  /** Column ATTR_VALUE. In UTF-8 */
  char m_attr_value[MAX_ATTR_VALUE_CHARS * MAX_UTF8_BYTES];
  /** Length in bytes of @c m_attr_name. */
  uint m_attr_value_length;
  /** Column ORDINAL_POSITION. */
  ulong m_ordinal_position;
};

/** Abstract table PERFORMANCE_SCHEMA.SESSION_CONNECT_ATTRS. */
class table_session_connect : public cursor_by_thread_connect_attr
{
protected:
  table_session_connect(const PFS_engine_table_share *share);

public:
  ~table_session_connect();

protected:
  void make_row(PFS_thread *pfs, uint ordinal) override;
  virtual bool thread_fits(PFS_thread *thread);
  int read_row_values(TABLE *table, unsigned char *buf,
                      Field **fields, bool read_all) override;
protected:
  /** Current row. */
  row_session_connect_attrs m_row;
  /** Safe copy of @c PFS_thread::m_session_connect_attrs. */
  char *m_copy_session_connect_attrs;
  /** Safe copy of @c PFS_thread::m_session_connect_attrs_length. */
  uint m_copy_session_connect_attrs_length;
};

/** @} */
#endif
