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

#ifndef CURSOR_BY_THREAD_CONNECT_ATTR_H
#define CURSOR_BY_THREAD_CONNECT_ATTR_H

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "pfs_instr.h"

/**
  \addtogroup Performance_schema_tables
  @{
*/

/**
  Position of a cursor on abstract table
  PERFORMANCE_SCHEMA.SESSION_CONNECT_ATTRS.
*/
struct pos_connect_attr_by_thread_by_attr
: public PFS_double_index
{
  pos_connect_attr_by_thread_by_attr()
    : PFS_double_index(0, 0)
  {}

  inline void next_thread(void)
  {
    m_index_1++;
    m_index_2= 0;
  }

  inline void reset(void)
  {
    m_index_1= 0;
    m_index_2= 0;
  }
};

/** Cursor CURSOR_BY_THREAD_CONNECT_ATTR. */
class cursor_by_thread_connect_attr : public PFS_engine_table
{
public:
  static ha_rows get_row_count();

  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position(void) override;

protected:
  cursor_by_thread_connect_attr(const PFS_engine_table_share *share);

public:
  ~cursor_by_thread_connect_attr() = default;

protected:
  virtual void make_row(PFS_thread *thread, uint ordinal)= 0;
  /** True if row exists */
  bool m_row_exists;

private:
  /** Current position. */
  pos_connect_attr_by_thread_by_attr m_pos;
  /** Next position. */
  pos_connect_attr_by_thread_by_attr m_next_pos;
};

/** @} */
#endif
