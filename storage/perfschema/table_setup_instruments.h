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

#ifndef TABLE_SETUP_INSTRUMENTS_H
#define TABLE_SETUP_INSTRUMENTS_H

/**
  @file storage/perfschema/table_setup_instruments.h
  Table SETUP_INSTRUMENTS (declarations).
*/

#include "pfs_instr_class.h"
#include "pfs_engine_table.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.SETUP_INSTRUMENTS. */
struct row_setup_instruments
{
  /** Columns NAME, ENABLED, TIMED. */
  PFS_instr_class *m_instr_class;
  /** True if column ENABLED can be updated. */
  bool m_update_enabled;
  /** True if column TIMED can be updated. */
  bool m_update_timed;
};

/** Position of a cursor on PERFORMANCE_SCHEMA.SETUP_INSTRUMENTS. */
struct pos_setup_instruments : public PFS_double_index
{
  static const uint FIRST_VIEW= 1;
  static const uint VIEW_MUTEX= 1;
  static const uint VIEW_RWLOCK= 2;
  static const uint VIEW_COND= 3;
  static const uint VIEW_THREAD= 4;
  static const uint VIEW_FILE= 5;
  static const uint VIEW_TABLE= 6;
  static const uint VIEW_STAGE= 7;
  static const uint VIEW_STATEMENT= 8;
  static const uint VIEW_TRANSACTION=9;
  static const uint VIEW_SOCKET= 10;
  static const uint VIEW_IDLE= 11;
  static const uint VIEW_BUILTIN_MEMORY= 12;
  static const uint VIEW_MEMORY= 13;
  static const uint VIEW_METADATA= 14;
  static const uint LAST_VIEW= 14;

  pos_setup_instruments()
    : PFS_double_index(FIRST_VIEW, 1)
  {}

  inline void reset(void)
  {
    m_index_1= FIRST_VIEW;
    m_index_2= 1;
  }

  inline bool has_more_view(void)
  { return (m_index_1 <= LAST_VIEW); }

  inline void next_view(void)
  {
    m_index_1++;
    m_index_2= 1;
  }
};

/** Table PERFORMANCE_SCHEMA.SETUP_INSTRUMENTS. */
class table_setup_instruments : public PFS_engine_table
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
    
  table_setup_instruments();

public:
  ~table_setup_instruments() = default;

private:
  void make_row(PFS_instr_class *klass, bool update_enabled, bool update_timed);

  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Current row. */
  row_setup_instruments m_row;
  /** Current position. */
  pos_setup_instruments m_pos;
  /** Next position. */
  pos_setup_instruments m_next_pos;
};

/** @} */
#endif
