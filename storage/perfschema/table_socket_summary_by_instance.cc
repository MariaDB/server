/* Copyright (c) 2008, 2021, Oracle and/or its affiliates.

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
  @file storage/perfschema/table_socket_summary_by_instance.cc
  Table SOCKET_SUMMARY_BY_INSTANCE (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_socket_summary_by_instance.h"
#include "pfs_global.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_socket_summary_by_instance::m_table_lock;

PFS_engine_table_share
table_socket_summary_by_instance::m_share=
{
  { C_STRING_WITH_LEN("socket_summary_by_instance") },
  &pfs_readonly_acl,
  table_socket_summary_by_instance::create,
  NULL, /* write_row */
  table_socket_summary_by_instance::delete_all_rows,
  table_socket_summary_by_instance::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE socket_summary_by_instance("
                      "EVENT_NAME VARCHAR(128) not null comment 'Socket instrument.',"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null comment 'Address in memory.',"
                      "COUNT_STAR BIGINT unsigned not null comment 'Number of summarized events',"
                      "SUM_TIMER_WAIT BIGINT unsigned not null comment 'Total wait time of the summarized events that are timed.',"
                      "MIN_TIMER_WAIT BIGINT unsigned not null comment 'Minimum wait time of the summarized events that are timed.',"
                      "AVG_TIMER_WAIT BIGINT unsigned not null comment 'Average wait time of the summarized events that are timed.',"
                      "MAX_TIMER_WAIT BIGINT unsigned not null comment 'Maximum wait time of the summarized events that are timed.',"
                      "COUNT_READ BIGINT unsigned not null comment 'Number of all read operations, including RECV, RECVFROM, and RECVMSG.',"
                      "SUM_TIMER_READ BIGINT unsigned not null comment 'Total wait time of all read operations that are timed.',"
                      "MIN_TIMER_READ BIGINT unsigned not null comment 'Minimum wait time of all read operations that are timed.',"
                      "AVG_TIMER_READ BIGINT unsigned not null comment 'Average wait time of all read operations that are timed.',"
                      "MAX_TIMER_READ BIGINT unsigned not null comment 'Maximum wait time of all read operations that are timed.',"
                      "SUM_NUMBER_OF_BYTES_READ BIGINT unsigned not null comment 'Bytes read by read operations.',"
                      "COUNT_WRITE BIGINT unsigned not null comment 'Number of all write operations, including SEND, SENDTO, and SENDMSG.',"
                      "SUM_TIMER_WRITE BIGINT unsigned not null comment 'Total wait time of all write operations that are timed.',"
                      "MIN_TIMER_WRITE BIGINT unsigned not null comment 'Minimum wait time of all write operations that are timed.',"
                      "AVG_TIMER_WRITE BIGINT unsigned not null comment 'Average wait time of all write operations that are timed.',"
                      "MAX_TIMER_WRITE BIGINT unsigned not null comment 'Maximum wait time of all write operations that are timed.',"
                      "SUM_NUMBER_OF_BYTES_WRITE BIGINT unsigned not null comment 'Bytes written by write operations.',"
                      "COUNT_MISC BIGINT unsigned not null comment 'Number of all miscellaneous operations not counted above, including CONNECT, LISTEN, ACCEPT, CLOSE, and SHUTDOWN.',"
                      "SUM_TIMER_MISC BIGINT unsigned not null comment 'Total wait time of all miscellaneous operations that are timed.',"
                      "MIN_TIMER_MISC BIGINT unsigned not null comment 'Minimum wait time of all miscellaneous operations that are timed.',"
                      "AVG_TIMER_MISC BIGINT unsigned not null comment 'Average wait time of all miscellaneous operations that are timed.',"
                      "MAX_TIMER_MISC BIGINT unsigned not null comment 'Maximum wait time of all miscellaneous operations that are timed.')") },
  false  /* perpetual */
};

PFS_engine_table* table_socket_summary_by_instance::create(void)
{
  return new table_socket_summary_by_instance();
}

table_socket_summary_by_instance::table_socket_summary_by_instance()
  : PFS_engine_table(&m_share, &m_pos),
  m_row_exists(false), m_pos(0), m_next_pos(0)
{}

int table_socket_summary_by_instance::delete_all_rows(void)
{
  reset_socket_instance_io();
  return 0;
}

ha_rows
table_socket_summary_by_instance::get_row_count(void)
{
  return global_socket_container.get_row_count();
}

void table_socket_summary_by_instance::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_socket_summary_by_instance::rnd_next(void)
{
  PFS_socket *pfs;

  m_pos.set_at(&m_next_pos);
  PFS_socket_iterator it= global_socket_container.iterate(m_pos.m_index);
  pfs= it.scan_next(& m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_socket_summary_by_instance::rnd_pos(const void *pos)
{
  PFS_socket *pfs;

  set_position(pos);

  pfs= global_socket_container.get(m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}

void table_socket_summary_by_instance::make_row(PFS_socket *pfs)
{
  pfs_optimistic_state lock;
  PFS_socket_class *safe_class;

  m_row_exists= false;

  /* Protect this reader against a socket delete */
  pfs->m_lock.begin_optimistic_lock(&lock);

  safe_class= sanitize_socket_class(pfs->m_class);
  if (unlikely(safe_class == NULL))
    return;

  m_row.m_event_name.make_row(safe_class);
  m_row.m_identity= pfs->m_identity;

  time_normalizer *normalizer= time_normalizer::get(wait_timer);

  /* Collect timer and byte count stats */
  m_row.m_io_stat.set(normalizer, &pfs->m_socket_stat.m_io_stat);

  if (!pfs->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
}

int table_socket_summary_by_instance::read_row_values(TABLE *table,
                                          unsigned char *,
                                          Field **fields,
                                          bool read_all)
{
  Field *f;

  if (unlikely(!m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case  0: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      case  1: /* OBJECT_INSTANCE */
        set_field_ulonglong(f, (intptr)m_row.m_identity);
        break;

      case  2:/* COUNT_STAR */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_count);
        break;
      case  3:/* SUM_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_sum);
        break;
      case  4: /* MIN_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_min);
        break;
      case  5: /* AVG_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_avg);
        break;
      case  6: /* MAX_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_max);
        break;

      case  7: /* COUNT_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_count);
        break;
      case  8: /* SUM_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_sum);
        break;
      case  9: /* MIN_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_min);
        break;
      case 10: /* AVG_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_avg);
        break;
      case 11: /* MAX_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_max);
        break;
      case 12: /* SUM_NUMBER_OF_BYTES_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_bytes);
        break;

      case 13: /* COUNT_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_count);
        break;
      case 14: /* SUM_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_sum);
        break;
      case 15: /* MIN_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_min);
        break;
      case 16: /* AVG_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_avg);
        break;
      case 17: /* MAX_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_max);
        break;
      case 18: /* SUM_NUMBER_OF_BYTES_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_bytes);
        break;

      case 19: /* COUNT_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_count);
        break;
      case 20: /* SUM_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_sum);
        break;
      case 21: /* MIN_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_min);
        break;
      case 22: /* AVG_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_avg);
        break;
      case 23: /* MAX_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_max);
        break;
      default:
        assert(false);
        break;
      }
    }
  }

  return 0;
}
