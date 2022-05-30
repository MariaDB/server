/* Copyright (c) 2014, 2021, Oracle and/or its affiliates.

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
  @file storage/perfschema/table_prepared_stmt_instances.cc
  Table PREPARED_STATEMENTS_INSTANCES (implementation).
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
#include "pfs_prepared_stmt.h"
#include "table_prepared_stmt_instances.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_prepared_stmt_instances::m_table_lock;

PFS_engine_table_share
table_prepared_stmt_instances::m_share=
{
  { C_STRING_WITH_LEN("prepared_statements_instances") },
  &pfs_truncatable_acl,
  table_prepared_stmt_instances::create,
  NULL, /* write_row */
  table_prepared_stmt_instances::delete_all_rows,
  table_prepared_stmt_instances::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE prepared_statements_instances("
  "OBJECT_INSTANCE_BEGIN bigint(20) unsigned NOT NULL comment 'The address in memory of the instrumented prepared statement.',"
  "STATEMENT_ID bigint(20) unsigned NOT NULL comment 'The internal statement ID assigned by the server.',"
  "STATEMENT_NAME varchar(64) default NULL comment 'For the binary protocol, this column is NULL. For the text protocol, this column is the external statement name assigned by the user.',"
  "SQL_TEXT longtext NOT NULL comment 'The prepared statement text, with ? placeholder markers.',"
  "OWNER_THREAD_ID bigint(20) unsigned NOT NULL comment 'Event thread id that created the prepared statement.',"
  "OWNER_EVENT_ID bigint(20) unsigned NOT NULL comment 'Event id that created the prepared statement.',"
  "OWNER_OBJECT_TYPE enum('EVENT','FUNCTION','PROCEDURE','TABLE','TRIGGER') DEFAULT NULL comment 'NULL for a prepared statement created by a client session. Type of the stored program that created the prepared statement.',"
  "OWNER_OBJECT_SCHEMA varchar(64) DEFAULT NULL comment 'NULL for a prepared statement created by a client session. Schema of the stored program that created the prepared statement.',"
  "OWNER_OBJECT_NAME varchar(64) DEFAULT NULL comment 'NULL for a prepared statement created by a client session. Name of the stored program that created the prepared statement.',"
  "TIMER_PREPARE bigint(20) unsigned NOT NULL comment 'The time spent executing the statement preparation itself.',"
  "COUNT_REPREPARE bigint(20) unsigned NOT NULL comment 'The number of times the statement was reprepared internally.',"
  "COUNT_EXECUTE bigint(20) unsigned NOT NULL comment 'Total times the prepared statement was executed.',"
  "SUM_TIMER_EXECUTE bigint(20) unsigned NOT NULL comment 'Total time spent executing all prepared statements.',"
  "MIN_TIMER_EXECUTE bigint(20) unsigned NOT NULL comment 'Minimum time spent executing any of the prepared statements.',"
  "AVG_TIMER_EXECUTE bigint(20) unsigned NOT NULL comment 'Average time spent executing any of the prepared statements.',"
  "MAX_TIMER_EXECUTE bigint(20) unsigned NOT NULL comment 'Maximum time spent executing any of the prepared statements.',"
  "SUM_LOCK_TIME bigint(20) unsigned NOT NULL comment 'The total time spent (in picoseconds) waiting for table locks for the prepared statements.',"
  "SUM_ERRORS bigint(20) unsigned NOT NULL comment 'The total number of errors that occurend for the prepared statements.',"
  "SUM_WARNINGS bigint(20) unsigned NOT NULL comment 'The total number of warnings that occurend for the prepared statements.',"
  "SUM_ROWS_AFFECTED bigint(20) unsigned NOT NULL comment 'The total number of affected rows by the prepared statements.',"
  "SUM_ROWS_SENT bigint(20) unsigned NOT NULL comment 'The total number of rows returned by the prepared statements.',"
  "SUM_ROWS_EXAMINED bigint(20) unsigned NOT NULL comment 'The total number of rows examined by the prepared statements.',"
  "SUM_CREATED_TMP_DISK_TABLES bigint(20) unsigned NOT NULL comment 'The total number of on-disk temporary tables created by the prepared statements.',"
  "SUM_CREATED_TMP_TABLES bigint(20) unsigned NOT NULL comment 'The total number of in-memory temporary tables created by the prepared statements.',"
  "SUM_SELECT_FULL_JOIN bigint(20) unsigned NOT NULL comment 'The total number of full joins executed by the prepared statements.',"
  "SUM_SELECT_FULL_RANGE_JOIN bigint(20) unsigned NOT NULL comment 'The total number of range search joins executed by the prepared statements.',"
  "SUM_SELECT_RANGE bigint(20) unsigned NOT NULL comment 'The total number of joins that used ranges on the first table executed by the prepared statements.',"
  "SUM_SELECT_RANGE_CHECK bigint(20) unsigned NOT NULL comment 'The total number of joins that check for key usage after each row executed by the prepared statements.',"
  "SUM_SELECT_SCAN bigint(20) unsigned NOT NULL comment 'The total number of joins that did a full scan of the first table executed by the prepared statements.',"
  "SUM_SORT_MERGE_PASSES bigint(20) unsigned NOT NULL comment 'The total number of merge passes that the sort algorithm has had to do for the prepared statements.',"
  "SUM_SORT_RANGE bigint(20) unsigned NOT NULL comment 'The total number of sorts that were done using ranges for the prepared statements.',"
  "SUM_SORT_ROWS bigint(20) unsigned NOT NULL comment 'The total number of sorted rows that were sorted by the prepared statements.',"
  "SUM_SORT_SCAN bigint(20) unsigned NOT NULL comment 'The total number of sorts that were done by scanning the table by the prepared statements.',"
  "SUM_NO_INDEX_USED bigint(20) unsigned NOT NULL comment 'The total number of statements that performed a table scan without using an index.',"
  "SUM_NO_GOOD_INDEX_USED bigint(20) unsigned NOT NULL comment 'The total number of statements where no good index was found.')")},
  false  /* perpetual */
};

PFS_engine_table*
table_prepared_stmt_instances::create(void)
{
  return new table_prepared_stmt_instances();
}

int
table_prepared_stmt_instances::delete_all_rows(void)
{
  reset_prepared_stmt_instances();
  return 0;
}

ha_rows
table_prepared_stmt_instances::get_row_count(void)
{
  return global_prepared_stmt_container.get_row_count();
}

table_prepared_stmt_instances::table_prepared_stmt_instances()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_prepared_stmt_instances::reset_position(void)
{
  m_pos= 0;
  m_next_pos= 0;
}

int table_prepared_stmt_instances::rnd_next(void)
{
  PFS_prepared_stmt* pfs;

  m_pos.set_at(&m_next_pos);
  PFS_prepared_stmt_iterator it= global_prepared_stmt_container.iterate(m_pos.m_index);
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
table_prepared_stmt_instances::rnd_pos(const void *pos)
{
  PFS_prepared_stmt* pfs;

  set_position(pos);

  pfs= global_prepared_stmt_container.get(m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}


void table_prepared_stmt_instances::make_row(PFS_prepared_stmt* prepared_stmt)
{
  pfs_optimistic_state lock;
  m_row_exists= false;

  prepared_stmt->m_lock.begin_optimistic_lock(&lock);

  m_row.m_identity= prepared_stmt->m_identity;

  m_row.m_stmt_id= prepared_stmt->m_stmt_id;

  m_row.m_owner_thread_id= prepared_stmt->m_owner_thread_id;
  m_row.m_owner_event_id= prepared_stmt->m_owner_event_id;

  m_row.m_stmt_name_length= prepared_stmt->m_stmt_name_length;
  if(m_row.m_stmt_name_length > 0)
    memcpy(m_row.m_stmt_name, prepared_stmt->m_stmt_name,
           m_row.m_stmt_name_length);

  m_row.m_sql_text_length= prepared_stmt->m_sqltext_length;
  if(m_row.m_sql_text_length > 0)
    memcpy(m_row.m_sql_text, prepared_stmt->m_sqltext,
           m_row.m_sql_text_length);

  m_row.m_owner_object_type= prepared_stmt->m_owner_object_type;

  m_row.m_owner_object_name_length= prepared_stmt->m_owner_object_name_length;
  if(m_row.m_owner_object_name_length > 0)
    memcpy(m_row.m_owner_object_name, prepared_stmt->m_owner_object_name,
           m_row.m_owner_object_name_length);

  m_row.m_owner_object_schema_length= prepared_stmt->m_owner_object_schema_length;
  if(m_row.m_owner_object_schema_length > 0)
    memcpy(m_row.m_owner_object_schema, prepared_stmt->m_owner_object_schema,
           m_row.m_owner_object_schema_length);

  time_normalizer *normalizer= time_normalizer::get(statement_timer);
  /* Get prepared statement prepare stats. */
  m_row.m_prepare_stat.set(normalizer, & prepared_stmt->m_prepare_stat);
  /* Get prepared statement reprepare stats. */
  m_row.m_reprepare_stat.set(normalizer, & prepared_stmt->m_reprepare_stat);
  /* Get prepared statement execute stats. */
  m_row.m_execute_stat.set(normalizer, & prepared_stmt->m_execute_stat);

  if (! prepared_stmt->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
}

int table_prepared_stmt_instances
::read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /*
    Set the null bits.
  */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* OBJECT_INSTANCE_BEGIN */
        set_field_ulonglong(f, (intptr)m_row.m_identity);
        break;
      case 1: /* STATEMENT_ID */
        set_field_ulonglong(f, m_row.m_stmt_id);
        break;
      case 2: /* STATEMENT_NAME */
        if(m_row.m_stmt_name_length > 0)
          set_field_varchar_utf8(f, m_row.m_stmt_name,
                                 m_row.m_stmt_name_length);
        else
          f->set_null();
        break;
      case 3: /* SQL_TEXT */
        if(m_row.m_sql_text_length > 0)
          set_field_longtext_utf8(f, m_row.m_sql_text,
                                 m_row.m_sql_text_length);
        else
          f->set_null();
        break;
      case 4: /* OWNER_THREAD_ID */
        set_field_ulonglong(f, m_row.m_owner_thread_id);
        break;
      case 5: /* OWNER_EVENT_ID */
        if(m_row.m_owner_event_id > 0)
          set_field_ulonglong(f, m_row.m_owner_event_id);
        else
          f->set_null();
        break;
      case 6: /* OWNER_OBJECT_TYPE */
        if(m_row.m_owner_object_type != 0)
          set_field_enum(f, m_row.m_owner_object_type);
        else
          f->set_null();
        break;
      case 7: /* OWNER_OBJECT_SCHEMA */
        if(m_row.m_owner_object_schema_length > 0)
          set_field_varchar_utf8(f, m_row.m_owner_object_schema,
                                 m_row.m_owner_object_schema_length);
        else
          f->set_null();
        break;
      case 8: /* OWNER_OBJECT_NAME */
        if(m_row.m_owner_object_name_length > 0)
          set_field_varchar_utf8(f, m_row.m_owner_object_name,
                                 m_row.m_owner_object_name_length);
        else
          f->set_null();
        break;
      case 9:    /* TIMER_PREPARE */
        m_row.m_prepare_stat.set_field(1, f);
        break;
      case 10:   /* COUNT_REPREPARE */
        m_row.m_reprepare_stat.set_field(0, f);
        break;
      default: /* 14, ... COUNT/SUM/MIN/AVG/MAX */
        m_row.m_execute_stat.set_field(f->field_index - 11, f);
        break;
      }
    }
  }

  return 0;
}

