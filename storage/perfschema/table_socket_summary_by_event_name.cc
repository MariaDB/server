/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file storage/perfschema/table_socket_summary_by_event_name.cc
  Table SOCKET_EVENT_NAMES (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "pfs_instr.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_socket_summary_by_event_name.h"
#include "pfs_global.h"
#include "pfs_visitor.h"

THR_LOCK table_socket_summary_by_event_name::m_table_lock;

PFS_engine_table_share
table_socket_summary_by_event_name::m_share=
{
  { C_STRING_WITH_LEN("socket_summary_by_event_name") },
  &pfs_readonly_acl,
  &table_socket_summary_by_event_name::create,
  NULL, /* write_row */
  table_socket_summary_by_event_name::delete_all_rows,
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE socket_summary_by_event_name("
                      "EVENT_NAME VARCHAR(128) not null,"
                      "COUNT_STAR BIGINT unsigned not null,"
                      "SUM_TIMER_WAIT BIGINT unsigned not null,"
                      "MIN_TIMER_WAIT BIGINT unsigned not null,"
                      "AVG_TIMER_WAIT BIGINT unsigned not null,"
                      "MAX_TIMER_WAIT BIGINT unsigned not null,"
                      "COUNT_READ BIGINT unsigned not null,"
                      "SUM_TIMER_READ BIGINT unsigned not null,"
                      "MIN_TIMER_READ BIGINT unsigned not null,"
                      "AVG_TIMER_READ BIGINT unsigned not null,"
                      "MAX_TIMER_READ BIGINT unsigned not null,"
                      "SUM_NUMBER_OF_BYTES_READ BIGINT unsigned not null,"
                      "COUNT_WRITE BIGINT unsigned not null,"
                      "SUM_TIMER_WRITE BIGINT unsigned not null,"
                      "MIN_TIMER_WRITE BIGINT unsigned not null,"
                      "AVG_TIMER_WRITE BIGINT unsigned not null,"
                      "MAX_TIMER_WRITE BIGINT unsigned not null,"
                      "SUM_NUMBER_OF_BYTES_WRITE BIGINT unsigned not null,"
                      "COUNT_MISC BIGINT unsigned not null,"
                      "SUM_TIMER_MISC BIGINT unsigned not null,"
                      "MIN_TIMER_MISC BIGINT unsigned not null,"
                      "AVG_TIMER_MISC BIGINT unsigned not null,"
                      "MAX_TIMER_MISC BIGINT unsigned not null)") }
};

PFS_engine_table* table_socket_summary_by_event_name::create(void)
{
  return new table_socket_summary_by_event_name();
}

table_socket_summary_by_event_name::table_socket_summary_by_event_name()
  : PFS_engine_table(&m_share, &m_pos),
  m_row_exists(false), m_pos(1), m_next_pos(1)
{}

int table_socket_summary_by_event_name::delete_all_rows(void)
{
  reset_socket_instance_io();
  reset_socket_class_io();
  return 0;
}

void table_socket_summary_by_event_name::reset_position(void)
{
  m_pos.m_index= 1;
  m_next_pos.m_index= 1;
}

int table_socket_summary_by_event_name::rnd_next(void)
{
  PFS_socket_class *socket_class;

  m_pos.set_at(&m_next_pos);

  socket_class= find_socket_class(m_pos.m_index);
  if (socket_class)
  {
    make_row(socket_class);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_socket_summary_by_event_name::rnd_pos(const void *pos)
{
  PFS_socket_class *socket_class;

  set_position(pos);

  socket_class= find_socket_class(m_pos.m_index);
  if (socket_class)
  {
    make_row(socket_class);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}

void table_socket_summary_by_event_name::make_row(PFS_socket_class *socket_class)
{
  m_row.m_event_name.make_row(socket_class);

  PFS_instance_socket_io_stat_visitor visitor;
  PFS_instance_iterator::visit_socket_instances(socket_class, &visitor);

  time_normalizer *normalizer= time_normalizer::get(wait_timer);
  
  /* Collect timer and byte count stats */
  m_row.m_io_stat.set(normalizer, &visitor.m_socket_io_stat);
  m_row_exists= true;
}

int table_socket_summary_by_event_name::read_row_values(TABLE *table,
                                          unsigned char *,
                                          Field **fields,
                                          bool read_all)
{
  Field *f;

  if (unlikely(!m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case  0: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      case  1: /* COUNT_STAR */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_count);
        break;
      case  2: /* SUM_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_sum);
        break;
      case  3: /* MIN_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_min);
        break;
      case  4: /* AVG_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_avg);
        break;
      case  5: /* MAX_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_max);
        break;

      case  6: /* COUNT_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_count);
        break;
      case  7: /* SUM_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_sum);
        break;
      case  8: /* MIN_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_min);
        break;
      case  9: /* AVG_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_avg);
        break;
      case 10: /* MAX_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_max);
        break;
      case 11: /* SUM_NUMBER_OF_BYTES_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_bytes);
        break;

      case 12: /* COUNT_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_count);
        break;
      case 13: /* SUM_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_sum);
        break;
      case 14: /* MIN_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_min);
        break;
      case 15: /* AVG_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_avg);
        break;
      case 16: /* MAX_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_max);
        break;
      case 17: /* SUM_NUMBER_OF_BYTES_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_bytes);
        break;

      case 18: /* COUNT_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_count);
        break;
      case 19: /* SUM_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_sum);
        break;
      case 20: /* MIN_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_min);
        break;
      case 21: /* AVG_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_avg);
        break;
      case 22: /* MAX_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_max);
        break;

      default:
        DBUG_ASSERT(false);
        break;
      }
    } // if
  } // for

  return 0;
}

