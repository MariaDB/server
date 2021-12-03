/* Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.

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

#ifndef TABLE_FILE_INSTANCES_H
#define TABLE_FILE_INSTANCES_H

/**
  @file storage/perfschema/table_file_instances.h
  Table FILE_INSTANCES (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.FILE_INSTANCES. */
struct row_file_instances
{
  /** Column FILE_NAME. */
  const char *m_filename;
  /** Length in bytes of @c m_filename. */
  uint m_filename_length;
  /** Column EVENT_NAME. */
  const char *m_event_name;
  /** Length in bytes of @c m_event_name. */
  uint m_event_name_length;
  /** Column OPEN_COUNT. */
  uint m_open_count;
};

/** Table PERFORMANCE_SCHEMA.FILE_INSTANCES. */
class table_file_instances : public PFS_engine_table
{
public:
  /** Table share */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();

  virtual int rnd_next();
  virtual int rnd_pos(const void *pos);
  virtual void reset_position(void);

private:
  virtual int read_row_values(TABLE *table,
                              unsigned char *buf,
                              Field **fields,
                              bool read_all);

  table_file_instances();

public:
  ~table_file_instances()
  {}

private:
  void make_row(PFS_file *pfs);

  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current row. */
  row_file_instances m_row;
  /** True if the current row exists. */
  bool m_row_exists;
  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;
};

/** @} */
#endif
