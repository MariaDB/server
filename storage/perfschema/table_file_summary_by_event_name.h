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
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef TABLE_FILE_SUMMARY_H
#define TABLE_FILE_SUMMARY_H

/**
  @file storage/perfschema/table_file_summary_by_event_name.h
  Table FILE_SUMMARY_BY_EVENT_NAME (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "table_helper.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.FILE_SUMMARY_BY_EVENT_NAME. */
struct row_file_summary_by_event_name
{
  /** Column EVENT_NAME. */
  PFS_event_name_row m_event_name;

  /** Columns COUNT_STAR, SUM/MIN/AVG/MAX TIMER and NUMBER_OF_BYTES
      for READ, WRITE and MISC operation types.
  */
  PFS_file_io_stat_row m_io_stat;
};

/** Table PERFORMANCE_SCHEMA.FILE_SUMMARY_BY_EVENT_NAME. */
class table_file_summary_by_event_name : public PFS_engine_table
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static int delete_all_rows();
  static ha_rows get_row_count();

  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

private:
  int read_row_values(TABLE *table,
                      unsigned char *buf,
                      Field **fields,
                      bool read_all) override;

  table_file_summary_by_event_name();

public:
  ~table_file_summary_by_event_name() = default;

private:
  void make_row(PFS_file_class *klass);

  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current row. */
  row_file_summary_by_event_name m_row;
  /** True if the current row exists. */
  bool m_row_exists;
  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;
};

/** @} */
#endif
