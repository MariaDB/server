/* Copyright (c) 2010, 2016, Oracle and/or its affiliates. All rights reserved.

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file storage/perfschema/table_esms_by_digest.cc
  Table EVENTS_STATEMENTS_SUMMARY_GLOBAL_BY_DIGEST (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_esms_by_digest.h"
#include "pfs_global.h"
#include "pfs_instr.h"
#include "pfs_timer.h"
#include "pfs_visitor.h"
#include "table_esms_by_digest.h"
#include "pfs_digest.h"

THR_LOCK table_esms_by_digest::m_table_lock;

PFS_engine_table_share
table_esms_by_digest::m_share=
{
  { C_STRING_WITH_LEN("events_statements_summary_by_digest") },
  &pfs_truncatable_acl,
  table_esms_by_digest::create,
  NULL, /* write_row */
  table_esms_by_digest::delete_all_rows,
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_summary_by_digest("
                      "SCHEMA_NAME VARCHAR(64) comment 'Database name. Records are summarised together with DIGEST.',"
                      "DIGEST VARCHAR(32) comment 'Performance Schema digest. Records are summarised together with SCHEMA NAME.',"
                      "DIGEST_TEXT LONGTEXT comment 'The unhashed form of the digest.',"
                      "COUNT_STAR BIGINT unsigned not null comment 'Number of summarized events',"
                      "SUM_TIMER_WAIT BIGINT unsigned not null comment 'Total wait time of the summarized events that are timed.',"
                      "MIN_TIMER_WAIT BIGINT unsigned not null comment 'Minimum wait time of the summarized events that are timed.',"
                      "AVG_TIMER_WAIT BIGINT unsigned not null comment 'Average wait time of the summarized events that are timed.',"
                      "MAX_TIMER_WAIT BIGINT unsigned not null comment 'Maximum wait time of the summarized events that are timed.',"
                      "SUM_LOCK_TIME BIGINT unsigned not null comment 'Sum of the LOCK_TIME column in the events_statements_current table.',"
                      "SUM_ERRORS BIGINT unsigned not null comment 'Sum of the ERRORS column in the events_statements_current table.',"
                      "SUM_WARNINGS BIGINT unsigned not null comment 'Sum of the WARNINGS column in the events_statements_current table.',"
                      "SUM_ROWS_AFFECTED BIGINT unsigned not null comment 'Sum of the ROWS_AFFECTED column in the events_statements_current table.',"
                      "SUM_ROWS_SENT BIGINT unsigned not null comment 'Sum of the ROWS_SENT column in the events_statements_current table.',"
                      "SUM_ROWS_EXAMINED BIGINT unsigned not null comment 'Sum of the ROWS_EXAMINED column in the events_statements_current table.',"
                      "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null comment 'Sum of the CREATED_TMP_DISK_TABLES column in the events_statements_current table.',"
                      "SUM_CREATED_TMP_TABLES BIGINT unsigned not null comment 'Sum of the CREATED_TMP_TABLES column in the events_statements_current table.',"
                      "SUM_SELECT_FULL_JOIN BIGINT unsigned not null comment 'Sum of the SELECT_FULL_JOIN column in the events_statements_current table.',"
                      "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null comment 'Sum of the SELECT_FULL_RANGE_JOIN column in the events_statements_current table.',"
                      "SUM_SELECT_RANGE BIGINT unsigned not null comment 'Sum of the SELECT_RANGE column in the events_statements_current table.',"
                      "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null comment 'Sum of the SELECT_RANGE_CHECK column in the events_statements_current table.',"
                      "SUM_SELECT_SCAN BIGINT unsigned not null comment 'Sum of the SELECT_SCAN column in the events_statements_current table.',"
                      "SUM_SORT_MERGE_PASSES BIGINT unsigned not null comment 'Sum of the SORT_MERGE_PASSES column in the events_statements_current table.',"
                      "SUM_SORT_RANGE BIGINT unsigned not null comment 'Sum of the SORT_RANGE column in the events_statements_current table.',"
                      "SUM_SORT_ROWS BIGINT unsigned not null comment 'Sum of the SORT_ROWS column in the events_statements_current table.',"
                      "SUM_SORT_SCAN BIGINT unsigned not null comment 'Sum of the SORT_SCAN column in the events_statements_current table.',"
                      "SUM_NO_INDEX_USED BIGINT unsigned not null comment 'Sum of the NO_INDEX_USED column in the events_statements_current table.',"
                      "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null comment 'Sum of the NO_GOOD_INDEX_USED column in the events_statements_current table.',"
                      "FIRST_SEEN TIMESTAMP(0) NOT NULL default 0 comment 'Time at which the digest was first seen.',"
                      "LAST_SEEN TIMESTAMP(0) NOT NULL default 0 comment 'Time at which the digest was most recently seen.')") }
};

PFS_engine_table*
table_esms_by_digest::create(void)
{
  return new table_esms_by_digest();
}

int
table_esms_by_digest::delete_all_rows(void)
{
  reset_esms_by_digest();
  return 0;
}

table_esms_by_digest::table_esms_by_digest()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_esms_by_digest::reset_position(void)
{
  m_pos= 0;
  m_next_pos= 0;
}

int table_esms_by_digest::rnd_next(void)
{
  PFS_statements_digest_stat* digest_stat;

  if (statements_digest_stat_array == NULL)
    return HA_ERR_END_OF_FILE;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < digest_max;
       m_pos.next())
  {
    digest_stat= &statements_digest_stat_array[m_pos.m_index];
    if (digest_stat->m_lock.is_populated())
    {
      if (digest_stat->m_first_seen != 0)
      {
        make_row(digest_stat);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_esms_by_digest::rnd_pos(const void *pos)
{
  PFS_statements_digest_stat* digest_stat;

  if (statements_digest_stat_array == NULL)
    return HA_ERR_END_OF_FILE;

  set_position(pos);
  digest_stat= &statements_digest_stat_array[m_pos.m_index];

  if (digest_stat->m_lock.is_populated())
  {
    if (digest_stat->m_first_seen != 0)
    {
      make_row(digest_stat);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}


void table_esms_by_digest::make_row(PFS_statements_digest_stat* digest_stat)
{
  m_row_exists= false;
  m_row.m_first_seen= digest_stat->m_first_seen;
  m_row.m_last_seen= digest_stat->m_last_seen;
  m_row.m_digest.make_row(digest_stat);

  /*
    Get statements stats.
  */
  time_normalizer *normalizer= time_normalizer::get(statement_timer);
  m_row.m_stat.set(normalizer, & digest_stat->m_stat);

  m_row_exists= true;
}

int table_esms_by_digest
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
      case 0: /* SCHEMA_NAME */
      case 1: /* DIGEST */
      case 2: /* DIGEST_TEXT */
        m_row.m_digest.set_field(f->field_index, f);
        break;
      case 27: /* FIRST_SEEN */
        set_field_timestamp(f, m_row.m_first_seen);
        break;
      case 28: /* LAST_SEEN */
        set_field_timestamp(f, m_row.m_last_seen);
        break;
      default: /* 3, ... COUNT/SUM/MIN/AVG/MAX */
        m_row.m_stat.set_field(f->field_index - 3, f);
        break;
      }
    }
  }

  return 0;
}

