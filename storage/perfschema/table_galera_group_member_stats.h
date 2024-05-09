/* Copyright (c) 2021-2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef TABLE_GALERA_GROUP_MEMBER_STATS_H
#define TABLE_GALERA_GROUP_MEMBER_STATS_H

#pragma once

/**
  @file storage/perfschema/table_galera_group_member_stats.h
  Table galera_group_member_stats (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "wsrep_ps.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** Table PERFORMANCE_SCHEMA.galera_group_member_stats. */
class table_galera_group_member_stats final : public PFS_engine_table
{
public:
  static PFS_engine_table_share_state m_share_state;
  /** Table share */
  static PFS_engine_table_share m_share;
  /** Table builder */
  static PFS_engine_table* create();

private:
  void make_row(uint index);
  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Fields definition. */
  static TABLE_FIELD_DEF m_field_def;
  /** True if the current row exists. */
  bool m_row_exists= false;
  /** Current row. */
  wsrep_node_stat_t m_row;
  /** Rows array. */
  wsrep_node_stat_t* m_entries= nullptr;
  /** Number of rows. */
  uint32_t m_rows= 0;
  /** Current position. */
  PFS_simple_index m_pos= 0;
  /** Next position. */
  PFS_simple_index m_next_pos= 0;

protected:
  /**
    Read the current row values.
    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */

  int read_row_values(TABLE *table, unsigned char *buf,
                      Field **fields, bool read_all) override;

  table_galera_group_member_stats();

public:
  ~table_galera_group_member_stats();

  static ha_rows get_row_count();

  int rnd_init(bool) override;
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position() override;
};

/** @} */

#endif
