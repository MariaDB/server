/*
      Copyright (c) 2013, 2018, Oracle and/or its affiliates. All rights reserved.

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
  @file storage/perfschema/table_replication_applier_status.cc
  Table replication_applier_status (implementation).
*/

//#define HAVE_REPLICATION

#include "my_global.h"

#ifdef HAVE_REPLICATION
#include "table_replication_applier_status.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "slave.h"
//#include "rpl_info.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
//#include "rpl_msr.h"    /*Multi source replication */

THR_LOCK table_replication_applier_status::m_table_lock;

PFS_engine_table_share
table_replication_applier_status::m_share=
{
  { C_STRING_WITH_LEN("replication_applier_status") },
  &pfs_readonly_acl,
  table_replication_applier_status::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_applier_status::get_row_count,    /* records */
  sizeof(pos_t), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE replication_applier_status("
  "CHANNEL_NAME CHAR(64) collate utf8_general_ci not null,"
  "SERVICE_STATE ENUM('ON','OFF') not null,"
  "REMAINING_DELAY INTEGER unsigned,"
  "COUNT_TRANSACTIONS_RETRIES BIGINT unsigned not null)") },
  false  /* perpetual */
};


PFS_engine_table* table_replication_applier_status::create(void)
{
  return new table_replication_applier_status();
}

table_replication_applier_status::table_replication_applier_status()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_applier_status::~table_replication_applier_status()
{}

void table_replication_applier_status::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_replication_applier_status::get_row_count()
{
 return master_info_index->master_info_hash.records;
}


int table_replication_applier_status::rnd_next(void)
{
  Master_info *mi;
  mysql_mutex_lock(&LOCK_active_mi);

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < master_info_index->master_info_hash.records;
       m_pos.next())
  {
    mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index);

    if (mi && mi->host[0])
    {
      make_row(mi);
      m_next_pos.set_after(&m_pos);
      mysql_mutex_unlock(&LOCK_active_mi);
      return 0;
    }
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  return HA_ERR_END_OF_FILE;
}


int table_replication_applier_status::rnd_pos(const void *pos)
{
  Master_info *mi=NULL;
  int res= HA_ERR_RECORD_DELETED;

  set_position(pos);

  mysql_mutex_lock(&LOCK_active_mi);

  if ((mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index)))
  {
    make_row(mi);
    res= 0;
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

void table_replication_applier_status::make_row(Master_info *mi)
{
  char *slave_sql_running_state= NULL;

  m_row_exists= false;

  DBUG_ASSERT(mi != NULL);

  m_row.channel_name_length= static_cast<uint>(mi->connection_name.length);
  memcpy(m_row.channel_name, mi->connection_name.str, m_row.channel_name_length);

  //mysql_mutex_lock(&mi->rli->info_thd_lock);

  slave_sql_running_state= const_cast<char *>
                           (mi->rli.sql_driver_thd ?
                            mi->rli.sql_driver_thd->get_proc_info() : "");
  //mysql_mutex_unlock(&mi->rli->info_thd_lock);


  mysql_mutex_lock(&mi->data_lock);
  mysql_mutex_lock(&mi->rli.data_lock);

  if (mi->rli.slave_running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  m_row.remaining_delay= 0;
  if (slave_sql_running_state == stage_sql_thd_waiting_until_delay.m_name)
  {
    time_t t= my_time(0), sql_delay_end= 0; //mi->rli.>get_sql_delay_end();
    m_row.remaining_delay= (uint)(t < sql_delay_end ?
                                      sql_delay_end - t : 0);
    m_row.remaining_delay_is_set= true;
  }
  else
    m_row.remaining_delay_is_set= false;

  m_row.count_transactions_retries= mi->rli.retried_trans;

  mysql_mutex_unlock(&mi->rli.data_lock);
  mysql_mutex_unlock(&mi->data_lock);

  m_row_exists= true;
}

int table_replication_applier_status::read_row_values(TABLE *table,
                                       unsigned char *buf,
                                       Field **fields,
                                       bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /**channel_name*/
         set_field_char_utf8(f, m_row.channel_name, m_row.channel_name_length);
         break;
      case 1: /* service_state */
        set_field_enum(f, m_row.service_state);
        break;
      case 2: /* remaining_delay */
        if (m_row.remaining_delay_is_set)
          set_field_ulong(f, m_row.remaining_delay);
        else
          f->set_null();
        break;
      case 3: /* total number of times transactions were retried */
        set_field_ulonglong(f, m_row.count_transactions_retries);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}
#endif
