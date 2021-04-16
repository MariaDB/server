/* Copyright (c) 2010, 2015, Oracle and/or its affiliates. All rights reserved.

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
  Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/table_esms_by_program.cc
  Table EVENTS_STATEMENTS_SUMMARY_BY_PROGRAM (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "pfs_global.h"
#include "pfs_instr.h"
#include "pfs_timer.h"
#include "pfs_visitor.h"
#include "pfs_program.h"
#include "table_esms_by_program.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_esms_by_program::m_table_lock;

PFS_engine_table_share
table_esms_by_program::m_share=
{
  { C_STRING_WITH_LEN("events_statements_summary_by_program") },
  &pfs_truncatable_acl,
  table_esms_by_program::create,
  NULL, /* write_row */
  table_esms_by_program::delete_all_rows,
  table_esms_by_program::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_summary_by_program ("
                      "OBJECT_TYPE enum('EVENT', 'FUNCTION', 'PROCEDURE', 'TABLE', 'TRIGGER'),"
                      "OBJECT_SCHEMA varchar(64) NOT NULL,"
                      "OBJECT_NAME varchar(64) NOT NULL,"
                      "COUNT_STAR bigint(20) unsigned NOT NULL,"
                      "SUM_TIMER_WAIT bigint(20) unsigned NOT NULL,"
                      "MIN_TIMER_WAIT bigint(20) unsigned NOT NULL,"
                      "AVG_TIMER_WAIT bigint(20) unsigned NOT NULL,"
                      "MAX_TIMER_WAIT bigint(20) unsigned NOT NULL,"
                      "COUNT_STATEMENTS bigint(20) unsigned NOT NULL,"
                      "SUM_STATEMENTS_WAIT bigint(20) unsigned NOT NULL,"
                      "MIN_STATEMENTS_WAIT bigint(20) unsigned NOT NULL,"
                      "AVG_STATEMENTS_WAIT bigint(20) unsigned NOT NULL,"
                      "MAX_STATEMENTS_WAIT bigint(20) unsigned NOT NULL,"
                      "SUM_LOCK_TIME bigint(20) unsigned NOT NULL,"
                      "SUM_ERRORS bigint(20) unsigned NOT NULL,"
                      "SUM_WARNINGS bigint(20) unsigned NOT NULL,"
                      "SUM_ROWS_AFFECTED bigint(20) unsigned NOT NULL,"
                      "SUM_ROWS_SENT bigint(20) unsigned NOT NULL,"
                      "SUM_ROWS_EXAMINED bigint(20) unsigned NOT NULL,"
                      "SUM_CREATED_TMP_DISK_TABLES bigint(20) unsigned NOT NULL,"
                      "SUM_CREATED_TMP_TABLES bigint(20) unsigned NOT NULL,"
                      "SUM_SELECT_FULL_JOIN bigint(20) unsigned NOT NULL,"
                      "SUM_SELECT_FULL_RANGE_JOIN bigint(20) unsigned NOT NULL,"
                      "SUM_SELECT_RANGE bigint(20) unsigned NOT NULL,"
                      "SUM_SELECT_RANGE_CHECK bigint(20) unsigned NOT NULL,"
                      "SUM_SELECT_SCAN bigint(20) unsigned NOT NULL,"
                      "SUM_SORT_MERGE_PASSES bigint(20) unsigned NOT NULL,"
                      "SUM_SORT_RANGE bigint(20) unsigned NOT NULL,"
                      "SUM_SORT_ROWS bigint(20) unsigned NOT NULL,"
                      "SUM_SORT_SCAN bigint(20) unsigned NOT NULL,"
                      "SUM_NO_INDEX_USED bigint(20) unsigned NOT NULL,"
                      "SUM_NO_GOOD_INDEX_USED bigint(20) unsigned NOT NULL)")},
  false  /* perpetual */
};

PFS_engine_table*
table_esms_by_program::create(void)
{
  return new table_esms_by_program();
}

int
table_esms_by_program::delete_all_rows(void)
{
  reset_esms_by_program();
  return 0;
}

ha_rows
table_esms_by_program::get_row_count(void)
{
  return global_program_container.get_row_count();
}

table_esms_by_program::table_esms_by_program()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_esms_by_program::reset_position(void)
{
  m_pos= 0;
  m_next_pos= 0;
}

int table_esms_by_program::rnd_next(void)
{
  PFS_program* pfs;

  m_pos.set_at(&m_next_pos);
  PFS_program_iterator it= global_program_container.iterate(m_pos.m_index);
  pfs= it.scan_next(& m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int
table_esms_by_program::rnd_pos(const void *pos)
{
  PFS_program* pfs;

  set_position(pos);

  pfs= global_program_container.get(m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}


void table_esms_by_program::make_row(PFS_program* program)
{
  pfs_optimistic_state lock;
  m_row_exists= false;

  program->m_lock.begin_optimistic_lock(&lock);

  m_row.m_object_type= program->m_type;

  m_row.m_object_name_length= program->m_object_name_length;
  if(m_row.m_object_name_length > 0)
    memcpy(m_row.m_object_name, program->m_object_name,
           m_row.m_object_name_length);

  m_row.m_schema_name_length= program->m_schema_name_length;
  if(m_row.m_schema_name_length > 0)
    memcpy(m_row.m_schema_name, program->m_schema_name,
           m_row.m_schema_name_length);

  time_normalizer *normalizer= time_normalizer::get(statement_timer);
  /* Get stored program's over all stats. */
  m_row.m_sp_stat.set(normalizer, &program->m_sp_stat);
  /* Get sub statements' stats. */
  m_row.m_stmt_stat.set(normalizer, & program->m_stmt_stat);

  if (! program->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
}

int table_esms_by_program
::read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /*
    Set the null bits. It indicates how many fields could be null
    in the table.
  */
  DBUG_ASSERT(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* OBJECT_TYPE */
        if(m_row.m_object_type != 0)
          set_field_enum(f, m_row.m_object_type);
        else
          f->set_null();
        break;
      case 1: /* OBJECT_SCHEMA */
        if(m_row.m_schema_name_length > 0)
          set_field_varchar_utf8(f, m_row.m_schema_name,
                                 m_row.m_schema_name_length);
        else
          f->set_null();
        break;
      case 2: /* OBJECT_NAME */
        if(m_row.m_object_name_length > 0)
          set_field_varchar_utf8(f, m_row.m_object_name,
                                 m_row.m_object_name_length);
        else
          f->set_null();
        break;
      case 3: /* COUNT_STAR */
      case 4: /* SUM_TIMER_WAIT */
      case 5: /* MIN_TIMER_WAIT */
      case 6: /* AVG_TIMER_WAIT */
      case 7: /* MAX_TIMER_WAIT */
        m_row.m_sp_stat.set_field(f->field_index - 3, f);
        break;
      default: /* 8, ... COUNT/SUM/MIN/AVG/MAX */
        m_row.m_stmt_stat.set_field(f->field_index - 8, f);
        break;
      }
    }
  }

  return 0;
}

