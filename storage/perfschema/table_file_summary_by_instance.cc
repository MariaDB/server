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
  @file storage/perfschema/table_file_summary_by_instance.cc
  Table FILE_SUMMARY_BY_INSTANCE (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_file_summary_by_instance.h"
#include "pfs_global.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_file_summary_by_instance::m_table_lock;

PFS_engine_table_share
table_file_summary_by_instance::m_share=
{
  { C_STRING_WITH_LEN("file_summary_by_instance") },
  &pfs_truncatable_acl,
  table_file_summary_by_instance::create,
  NULL, /* write_row */
  table_file_summary_by_instance::delete_all_rows,
  table_file_summary_by_instance::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE file_summary_by_instance("
                      "FILE_NAME VARCHAR(512) not null comment 'File name.',"
                      "EVENT_NAME VARCHAR(128) not null comment 'Event name.',"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null comment 'Address in memory. Together with FILE_NAME and EVENT_NAME uniquely identifies a row.',"
                      "COUNT_STAR BIGINT unsigned not null comment 'Number of summarized events',"
                      "SUM_TIMER_WAIT BIGINT unsigned not null comment 'Total wait time of the summarized events that are timed.',"
                      "MIN_TIMER_WAIT BIGINT unsigned not null comment 'Minimum wait time of the summarized events that are timed.',"
                      "AVG_TIMER_WAIT BIGINT unsigned not null comment 'Average wait time of the summarized events that are timed.',"
                      "MAX_TIMER_WAIT BIGINT unsigned not null comment 'Maximum wait time of the summarized events that are timed.',"
                      "COUNT_READ BIGINT unsigned not null comment 'Number of all read operations, including FGETS, FGETC, FREAD, and READ.',"
                      "SUM_TIMER_READ BIGINT unsigned not null comment 'Total wait time of all read operations that are timed.',"
                      "MIN_TIMER_READ BIGINT unsigned not null comment 'Minimum wait time of all read operations that are timed.',"
                      "AVG_TIMER_READ BIGINT unsigned not null comment 'Average wait time of all read operations that are timed.',"
                      "MAX_TIMER_READ BIGINT unsigned not null comment 'Maximum wait time of all read operations that are timed.',"
                      "SUM_NUMBER_OF_BYTES_READ BIGINT not null comment 'Bytes read by read operations.',"
                      "COUNT_WRITE BIGINT unsigned not null comment 'Number of all write operations, including FPUTS, FPUTC, FPRINTF, VFPRINTF, FWRITE, and PWRITE.',"
                      "SUM_TIMER_WRITE BIGINT unsigned not null comment 'Total wait time of all write operations that are timed.',"
                      "MIN_TIMER_WRITE BIGINT unsigned not null comment 'Minimum wait time of all write operations that are timed.',"
                      "AVG_TIMER_WRITE BIGINT unsigned not null comment 'Average wait time of all write operations that are timed.',"
                      "MAX_TIMER_WRITE BIGINT unsigned not null comment 'Maximum wait time of all write operations that are timed.',"
                      "SUM_NUMBER_OF_BYTES_WRITE BIGINT not null comment 'Bytes written by write operations.',"
                      "COUNT_MISC BIGINT unsigned not null comment 'Number of all miscellaneous operations not counted above, including CREATE, DELETE, OPEN, CLOSE, STREAM_OPEN, STREAM_CLOSE, SEEK, TELL, FLUSH, STAT, FSTAT, CHSIZE, RENAME, and SYNC.',"
                      "SUM_TIMER_MISC BIGINT unsigned not null comment 'Total wait time of all miscellaneous operations that are timed.',"
                      "MIN_TIMER_MISC BIGINT unsigned not null comment 'Minimum wait time of all miscellaneous operations that are timed.',"
                      "AVG_TIMER_MISC BIGINT unsigned not null comment 'Average wait time of all miscellaneous operations that are timed.',"
                      "MAX_TIMER_MISC BIGINT unsigned not null comment 'Maximum wait time of all miscellaneous operations that are timed.')") },
  false  /* perpetual */
};

PFS_engine_table* table_file_summary_by_instance::create(void)
{
  return new table_file_summary_by_instance();
}

int table_file_summary_by_instance::delete_all_rows(void)
{
  reset_file_instance_io();
  return 0;
}

ha_rows
table_file_summary_by_instance::get_row_count(void)
{
  return global_file_container.get_row_count();
}

table_file_summary_by_instance::table_file_summary_by_instance()
  : PFS_engine_table(&m_share, &m_pos),
  m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_file_summary_by_instance::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_file_summary_by_instance::rnd_next(void)
{
  PFS_file *pfs;

  m_pos.set_at(&m_next_pos);
  PFS_file_iterator it= global_file_container.iterate(m_pos.m_index);
  pfs= it.scan_next(& m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_file_summary_by_instance::rnd_pos(const void *pos)
{
  PFS_file *pfs;

  set_position(pos);

  pfs= global_file_container.get(m_pos.m_index);
  if (pfs != NULL)
  {
    make_row(pfs);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}

/**
  Build a row.
  @param pfs              the file the cursor is reading
*/
void table_file_summary_by_instance::make_row(PFS_file *pfs)
{
  pfs_optimistic_state lock;
  PFS_file_class *safe_class;

  m_row_exists= false;

  /* Protect this reader against a file delete */
  pfs->m_lock.begin_optimistic_lock(&lock);

  safe_class= sanitize_file_class(pfs->m_class);
  if (unlikely(safe_class == NULL))
    return;

  m_row.m_filename= pfs->m_filename;
  m_row.m_filename_length= pfs->m_filename_length;
  m_row.m_event_name.make_row(safe_class);
  m_row.m_identity= pfs->m_identity;

  time_normalizer *normalizer= time_normalizer::get(wait_timer);

  /* Collect timer and byte count stats */
  m_row.m_io_stat.set(normalizer, &pfs->m_file_stat.m_io_stat);

  if (pfs->m_lock.end_optimistic_lock(&lock))
    m_row_exists= true;
}

int table_file_summary_by_instance::read_row_values(TABLE *table,
                                                          unsigned char *,
                                                          Field **fields,
                                                          bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case  0: /* FILE_NAME */
        set_field_varchar_utf8(f, m_row.m_filename, m_row.m_filename_length);
        break;
      case  1: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      case  2: /* OBJECT_INSTANCE */
        set_field_ulonglong(f, (ulonglong)m_row.m_identity);
        break;

      case  3:/* COUNT_STAR */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_count);
        break;
      case  4:/* SUM_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_sum);
        break;
      case  5: /* MIN_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_min);
        break;
      case  6: /* AVG_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_avg);
        break;
      case  7: /* MAX_TIMER_WAIT */
        set_field_ulonglong(f, m_row.m_io_stat.m_all.m_waits.m_max);
        break;

      case  8: /* COUNT_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_count);
        break;
      case  9: /* SUM_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_sum);
        break;
      case 10: /* MIN_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_min);
        break;
      case 11: /* AVG_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_avg);
        break;
      case 12: /* MAX_TIMER_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_waits.m_max);
        break;
      case 13: /* SUM_NUMBER_OF_BYTES_READ */
        set_field_ulonglong(f, m_row.m_io_stat.m_read.m_bytes);
        break;

      case 14: /* COUNT_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_count);
        break;
      case 15: /* SUM_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_sum);
        break;
      case 16: /* MIN_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_min);
        break;
      case 17: /* AVG_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_avg);
        break;
      case 18: /* MAX_TIMER_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_waits.m_max);
        break;
      case 19: /* SUM_NUMBER_OF_BYTES_WRITE */
        set_field_ulonglong(f, m_row.m_io_stat.m_write.m_bytes);
        break;

      case 20: /* COUNT_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_count);
        break;
      case 21: /* SUM_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_sum);
        break;
      case 22: /* MIN_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_min);
        break;
      case 23: /* AVG_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_avg);
        break;
      case 24: /* MAX_TIMER_MISC */
        set_field_ulonglong(f, m_row.m_io_stat.m_misc.m_waits.m_max);
        break;
      default:
        assert(false);
      }
    }
  }

  return 0;
}

