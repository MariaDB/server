/* Copyright (c) 2010, 2011, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_tiws_by_index_usage.cc
  Table TABLE_IO_WAITS_SUMMARY_BY_INDEX_USAGE (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_tiws_by_index_usage.h"
#include "pfs_global.h"
#include "pfs_visitor.h"

THR_LOCK table_tiws_by_index_usage::m_table_lock;

PFS_engine_table_share
table_tiws_by_index_usage::m_share=
{
  { C_STRING_WITH_LEN("table_io_waits_summary_by_index_usage") },
  &pfs_truncatable_acl,
  table_tiws_by_index_usage::create,
  NULL, /* write_row */
  table_tiws_by_index_usage::delete_all_rows,
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(pos_tiws_by_index_usage),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE table_io_waits_summary_by_index_usage("
                      "OBJECT_TYPE VARCHAR(64),"
                      "OBJECT_SCHEMA VARCHAR(64),"
                      "OBJECT_NAME VARCHAR(64),"
                      "INDEX_NAME VARCHAR(64),"
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
                      "COUNT_WRITE BIGINT unsigned not null,"
                      "SUM_TIMER_WRITE BIGINT unsigned not null,"
                      "MIN_TIMER_WRITE BIGINT unsigned not null,"
                      "AVG_TIMER_WRITE BIGINT unsigned not null,"
                      "MAX_TIMER_WRITE BIGINT unsigned not null,"
                      "COUNT_FETCH BIGINT unsigned not null,"
                      "SUM_TIMER_FETCH BIGINT unsigned not null,"
                      "MIN_TIMER_FETCH BIGINT unsigned not null,"
                      "AVG_TIMER_FETCH BIGINT unsigned not null,"
                      "MAX_TIMER_FETCH BIGINT unsigned not null,"
                      "COUNT_INSERT BIGINT unsigned not null,"
                      "SUM_TIMER_INSERT BIGINT unsigned not null,"
                      "MIN_TIMER_INSERT BIGINT unsigned not null,"
                      "AVG_TIMER_INSERT BIGINT unsigned not null,"
                      "MAX_TIMER_INSERT BIGINT unsigned not null,"
                      "COUNT_UPDATE BIGINT unsigned not null,"
                      "SUM_TIMER_UPDATE BIGINT unsigned not null,"
                      "MIN_TIMER_UPDATE BIGINT unsigned not null,"
                      "AVG_TIMER_UPDATE BIGINT unsigned not null,"
                      "MAX_TIMER_UPDATE BIGINT unsigned not null,"
                      "COUNT_DELETE BIGINT unsigned not null,"
                      "SUM_TIMER_DELETE BIGINT unsigned not null,"
                      "MIN_TIMER_DELETE BIGINT unsigned not null,"
                      "AVG_TIMER_DELETE BIGINT unsigned not null,"
                      "MAX_TIMER_DELETE BIGINT unsigned not null)") }
};

PFS_engine_table*
table_tiws_by_index_usage::create(void)
{
  return new table_tiws_by_index_usage();
}

int
table_tiws_by_index_usage::delete_all_rows(void)
{
  reset_table_io_waits_by_table_handle();
  reset_table_io_waits_by_table();
  return 0;
}

table_tiws_by_index_usage::table_tiws_by_index_usage()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(), m_next_pos()
{}

void table_tiws_by_index_usage::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_tiws_by_index_usage::rnd_init(bool scan)
{
  m_normalizer= time_normalizer::get(wait_timer);
  return 0;
}

int table_tiws_by_index_usage::rnd_next(void)
{
  PFS_table_share *table_share;

  for (m_pos.set_at(&m_next_pos);
       m_pos.has_more_table();
       m_pos.next_table())
  {
    table_share= &table_share_array[m_pos.m_index_1];
    if (table_share->m_lock.is_populated())
    {
      uint safe_key_count= sanitize_index_count(table_share->m_key_count);
      if (m_pos.m_index_2 < safe_key_count)
      {
        make_row(table_share, m_pos.m_index_2);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
      if (m_pos.m_index_2 <= MAX_INDEXES)
      {
        m_pos.m_index_2= MAX_INDEXES;
        make_row(table_share, m_pos.m_index_2);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_tiws_by_index_usage::rnd_pos(const void *pos)
{
  PFS_table_share *table_share;

  set_position(pos);

  table_share= &table_share_array[m_pos.m_index_1];
  if (table_share->m_lock.is_populated())
  {
    uint safe_key_count= sanitize_index_count(table_share->m_key_count);
    if (m_pos.m_index_2 < safe_key_count)
    {
      make_row(table_share, m_pos.m_index_2);
      return 0;
    }
    if (m_pos.m_index_2 == MAX_INDEXES)
    {
      make_row(table_share, m_pos.m_index_2);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_tiws_by_index_usage::make_row(PFS_table_share *share, uint index)
{
  pfs_lock lock;

  m_row_exists= false;

  share->m_lock.begin_optimistic_lock(&lock);

  if (m_row.m_index.make_row(share, index))
    return;

  PFS_index_io_stat_visitor visitor;
  PFS_object_iterator::visit_table_indexes(share, index, & visitor);

  if (! share->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
  m_row.m_stat.set(m_normalizer, & visitor.m_stat);
}

int table_tiws_by_index_usage::read_row_values(TABLE *table,
                                         unsigned char *buf,
                                         Field **fields,
                                         bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* OBJECT_TYPE */
      case 1: /* SCHEMA_NAME */
      case 2: /* OBJECT_NAME */
      case 3: /* INDEX_NAME */
        m_row.m_index.set_field(f->field_index, f);
        break;
      case 4: /* COUNT_STAR */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_count);
        break;
      case 5: /* SUM */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_sum);
        break;
      case 6: /* MIN */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_min);
        break;
      case 7: /* AVG */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_avg);
        break;
      case 8: /* MAX */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_max);
        break;
      case 9: /* COUNT_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_count);
        break;
      case 10: /* SUM_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_sum);
        break;
      case 11: /* MIN_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_min);
        break;
      case 12: /* AVG_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_avg);
        break;
      case 13: /* MAX_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_max);
        break;
      case 14: /* COUNT_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_count);
        break;
      case 15: /* SUM_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_sum);
        break;
      case 16: /* MIN_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_min);
        break;
      case 17: /* AVG_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_avg);
        break;
      case 18: /* MAX_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_max);
        break;
      case 19: /* COUNT_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_count);
        break;
      case 20: /* SUM_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_sum);
        break;
      case 21: /* MIN_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_min);
        break;
      case 22: /* AVG_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_avg);
        break;
      case 23: /* MAX_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_max);
        break;
      case 24: /* COUNT_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_count);
        break;
      case 25: /* SUM_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_sum);
        break;
      case 26: /* MIN_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_min);
        break;
      case 27: /* AVG_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_avg);
        break;
      case 28: /* MAX_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_max);
        break;
      case 29: /* COUNT_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_count);
        break;
      case 30: /* SUM_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_sum);
        break;
      case 31: /* MIN_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_min);
        break;
      case 32: /* AVG_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_avg);
        break;
      case 33: /* MAX_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_max);
        break;
      case 34: /* COUNT_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_count);
        break;
      case 35: /* SUM_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_sum);
        break;
      case 36: /* MIN_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_min);
        break;
      case 37: /* AVG_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_avg);
        break;
      case 38: /* MAX_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_max);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

