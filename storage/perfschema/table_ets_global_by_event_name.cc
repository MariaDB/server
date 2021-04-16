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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_ets_global_by_event_name.cc
  Table EVENTS_TRANSACTIONS_SUMMARY_GLOBAL_BY_EVENT_NAME (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_ets_global_by_event_name.h"
#include "pfs_global.h"
#include "pfs_instr.h"
#include "pfs_timer.h"
#include "pfs_visitor.h"
#include "field.h"

THR_LOCK table_ets_global_by_event_name::m_table_lock;

PFS_engine_table_share
table_ets_global_by_event_name::m_share=
{
  { C_STRING_WITH_LEN("events_transactions_summary_global_by_event_name") },
  &pfs_truncatable_acl,
  table_ets_global_by_event_name::create,
  NULL, /* write_row */
  table_ets_global_by_event_name::delete_all_rows,
  table_ets_global_by_event_name::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_transactions_summary_global_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_READ_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_READ_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_READ_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_READ_WRITE BIGINT unsigned not null,"
  "COUNT_READ_ONLY BIGINT unsigned not null,"
  "SUM_TIMER_READ_ONLY BIGINT unsigned not null,"
  "MIN_TIMER_READ_ONLY BIGINT unsigned not null,"
  "AVG_TIMER_READ_ONLY BIGINT unsigned not null,"
  "MAX_TIMER_READ_ONLY BIGINT unsigned not null)")},
  false  /* perpetual */
};

PFS_engine_table*
table_ets_global_by_event_name::create(void)
{
  return new table_ets_global_by_event_name();
}

int
table_ets_global_by_event_name::delete_all_rows(void)
{
  reset_events_transactions_by_thread();
  reset_events_transactions_by_account();
  reset_events_transactions_by_user();
  reset_events_transactions_by_host();
  reset_events_transactions_global();
  return 0;
}

ha_rows
table_ets_global_by_event_name::get_row_count(void)
{
  return transaction_class_max;
}

table_ets_global_by_event_name::table_ets_global_by_event_name()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(1), m_next_pos(1)
{}

void table_ets_global_by_event_name::reset_position(void)
{
  m_pos= 1;
  m_next_pos= 1;
}

int table_ets_global_by_event_name::rnd_init(bool scan)
{
  m_normalizer= time_normalizer::get(transaction_timer);
  return 0;
}

int table_ets_global_by_event_name::rnd_next(void)
{
  PFS_transaction_class *transaction_class;

  m_pos.set_at(&m_next_pos);

  transaction_class= find_transaction_class(m_pos.m_index);
  if (transaction_class)
  {
    make_row(transaction_class);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int
table_ets_global_by_event_name::rnd_pos(const void *pos)
{
  PFS_transaction_class *transaction_class;

  set_position(pos);

  transaction_class=find_transaction_class(m_pos.m_index);
  if (transaction_class)
  {
    make_row(transaction_class);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}


void table_ets_global_by_event_name
::make_row(PFS_transaction_class *klass)
{
  m_row.m_event_name.make_row(klass);

  PFS_connection_transaction_visitor visitor(klass);
  PFS_connection_iterator::visit_global(true,  /* hosts */
                                        false, /* users */
                                        true,  /* accounts */
                                        true,  /* threads */
                                        false, /* THDs */
                                        & visitor);

  m_row.m_stat.set(m_normalizer, & visitor.m_stat);
  m_row_exists= true;
}

int table_ets_global_by_event_name
::read_row_values(TABLE *table, unsigned char *, Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* NAME */
        m_row.m_event_name.set_field(f);
        break;
      default:
        /**
          Columns COUNT_STAR, SUM/MIN/AVG/MAX_TIMER_WAIT,
          COUNT_READ_WRITE, SUM/MIN/AVG/MAX_TIMER_READ_WRITE,
          COUNT_READ_ONLY, SUM/MIN/AVG/MAX_TIMER_READ_ONLY
        */
        m_row.m_stat.set_field(f->field_index - 1, f);
        break;
      }
    }
  }

  return 0;
}

