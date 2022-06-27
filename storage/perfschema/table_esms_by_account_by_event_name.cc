/* Copyright (c) 2010, 2021, Oracle and/or its affiliates.

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
  @file storage/perfschema/table_esms_by_account_by_event_name.cc
  Table EVENTS_STATEMENTS_SUMMARY_BY_ACCOUNT_BY_EVENT_NAME (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_esms_by_account_by_event_name.h"
#include "pfs_global.h"
#include "pfs_visitor.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_esms_by_account_by_event_name::m_table_lock;

PFS_engine_table_share
table_esms_by_account_by_event_name::m_share=
{
  { C_STRING_WITH_LEN("events_statements_summary_by_account_by_event_name") },
  &pfs_truncatable_acl,
  table_esms_by_account_by_event_name::create,
  NULL, /* write_row */
  table_esms_by_account_by_event_name::delete_all_rows,
  table_esms_by_account_by_event_name::get_row_count,
  sizeof(pos_esms_by_account_by_event_name),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_summary_by_account_by_event_name("
                      "USER CHAR(" USERNAME_CHAR_LENGTH_STR ") collate utf8_bin default null comment 'User. Used together with HOST and EVENT_NAME for grouping events.',"
                      "HOST CHAR(" HOSTNAME_LENGTH_STR ") collate utf8_bin default null comment 'Host. Used together with USER and EVENT_NAME for grouping events.',"
                      "EVENT_NAME VARCHAR(128) not null comment 'Event name. Used together with USER and HOST for grouping events.',"
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
                      "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null comment 'Sum of the NO_GOOD_INDEX_USED column in the events_statements_current table.')") },
  false  /* perpetual */
};

PFS_engine_table*
table_esms_by_account_by_event_name::create(void)
{
  return new table_esms_by_account_by_event_name();
}

int
table_esms_by_account_by_event_name::delete_all_rows(void)
{
  reset_events_statements_by_thread();
  reset_events_statements_by_account();
  return 0;
}

ha_rows
table_esms_by_account_by_event_name::get_row_count(void)
{
  return global_account_container.get_row_count() * statement_class_max;
}

table_esms_by_account_by_event_name::table_esms_by_account_by_event_name()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(), m_next_pos()
{}

void table_esms_by_account_by_event_name::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_esms_by_account_by_event_name::rnd_init(bool scan)
{
  m_normalizer= time_normalizer::get(statement_timer);
  return 0;
}

int table_esms_by_account_by_event_name::rnd_next(void)
{
  PFS_account *account;
  PFS_statement_class *statement_class;
  bool has_more_account= true;

  for (m_pos.set_at(&m_next_pos);
       has_more_account;
       m_pos.next_account())
  {
    account= global_account_container.get(m_pos.m_index_1, & has_more_account);
    if (account != NULL)
    {
      statement_class= find_statement_class(m_pos.m_index_2);
      if (statement_class)
      {
        make_row(account, statement_class);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_esms_by_account_by_event_name::rnd_pos(const void *pos)
{
  PFS_account *account;
  PFS_statement_class *statement_class;

  set_position(pos);

  account= global_account_container.get(m_pos.m_index_1);
  if (account != NULL)
  {
    statement_class= find_statement_class(m_pos.m_index_2);
    if (statement_class)
    {
      make_row(account, statement_class);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_esms_by_account_by_event_name
::make_row(PFS_account *account, PFS_statement_class *klass)
{
  pfs_optimistic_state lock;
  m_row_exists= false;

  if (klass->is_mutable())
    return;

  account->m_lock.begin_optimistic_lock(&lock);

  if (m_row.m_account.make_row(account))
    return;

  m_row.m_event_name.make_row(klass);

  PFS_connection_statement_visitor visitor(klass);
  PFS_connection_iterator::visit_account(account,
                                         true,  /* threads */
                                         false, /* THDs */
                                         & visitor);

  if (! account->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
  m_row.m_stat.set(m_normalizer, & visitor.m_stat);
}

int table_esms_by_account_by_event_name
::read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* USER */
      case 1: /* HOST */
        m_row.m_account.set_field(f->field_index, f);
        break;
      case 2: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      default: /* 3, ... COUNT/SUM/MIN/AVG/MAX */
        m_row.m_stat.set_field(f->field_index - 3, f);
        break;
      }
    }
  }

  return 0;
}

