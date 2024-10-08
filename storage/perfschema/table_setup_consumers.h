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

#ifndef TABLE_SETUP_CONSUMERS_H
#define TABLE_SETUP_CONSUMERS_H

/**
  @file storage/perfschema/table_setup_consumers.h
  Table SETUP_CONSUMERS (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.SETUP_CONSUMERS. */
struct row_setup_consumers
{
  /** Column NAME. */
  LEX_STRING m_name;
  /** Column ENABLED. */
  bool *m_enabled_ptr;
  /** Hidden column, instrument refresh. */
  bool m_instrument_refresh;
  /** Hidden column, thread refresh. */
  bool m_thread_refresh;
};

/** Table PERFORMANCE_SCHEMA.SETUP_CONSUMERS. */
class table_setup_consumers : public PFS_engine_table
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static ha_rows get_row_count();

  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

protected:
  int read_row_values(TABLE *table,
                      unsigned char *buf,
                      Field **fields,
                      bool read_all) override;

  int update_row_values(TABLE *table,
                        const unsigned char *old_buf,
                        const unsigned char *new_buf,
                        Field **fields) override;

  table_setup_consumers();

public:
  ~table_setup_consumers() = default;

private:
  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current row. */
  row_setup_consumers *m_row;
  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;
};

/** @} */
#endif
