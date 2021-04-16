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
  @file storage/perfschema/table_replication_applier_status_by_cordinator.cc
  Table replication_applier_status_by_coordinator (implementation).
*/

//#define HAVE_REPLICATION

#include "my_global.h"

#ifdef HAVE_REPLICATION
#include "table_replication_applier_status_by_coordinator.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "slave.h"
//#include "rpl_info.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
//#include "rpl_msr.h"       /* Multisource replication */

THR_LOCK table_replication_applier_status_by_coordinator::m_table_lock;

PFS_engine_table_share
table_replication_applier_status_by_coordinator::m_share=
{
  { C_STRING_WITH_LEN("replication_applier_status_by_coordinator") },
  &pfs_readonly_acl,
  table_replication_applier_status_by_coordinator::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_applier_status_by_coordinator::get_row_count,
  sizeof(pos_t), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE replication_applier_status_by_coordinator("
  "CHANNEL_NAME CHAR(64) collate utf8_general_ci not null,"
  "THREAD_ID BIGINT UNSIGNED,"
  "SERVICE_STATE ENUM('ON','OFF') not null,"
  "LAST_ERROR_NUMBER INTEGER not null,"
  "LAST_ERROR_MESSAGE VARCHAR(1024) not null,"
  "LAST_ERROR_TIMESTAMP TIMESTAMP(0) not null)") },
  false  /* perpetual */
};

PFS_engine_table* table_replication_applier_status_by_coordinator::create(void)
{
  return new table_replication_applier_status_by_coordinator();
}

table_replication_applier_status_by_coordinator
  ::table_replication_applier_status_by_coordinator()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_applier_status_by_coordinator
  ::~table_replication_applier_status_by_coordinator()
{}

void table_replication_applier_status_by_coordinator::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_replication_applier_status_by_coordinator::get_row_count()
{
 return master_info_index->master_info_hash.records;
}


int table_replication_applier_status_by_coordinator::rnd_next(void)
{
  Master_info *mi;

  mysql_mutex_lock(&LOCK_active_mi);


  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < master_info_index->master_info_hash.records;
       m_pos.next())
  {
    mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index);

    /*
      Construct and display SQL Thread's (Coordinator) information in
      'replication_applier_status_by_coordinator' table only in the case of
      multi threaded slave mode. Code should do nothing in the case of single
      threaded slave mode. In case of single threaded slave mode SQL Thread's
      status will be reported as part of
      'replication_applier_status_by_worker' table.
    */
    if (mi && mi->host[0] && /*mi->rli.get_worker_count() > */ 0)
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

int table_replication_applier_status_by_coordinator::rnd_pos(const void *pos)
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

void table_replication_applier_status_by_coordinator::make_row(Master_info *mi)
{
  m_row_exists= false;

  DBUG_ASSERT(mi != NULL);

  mysql_mutex_lock(&mi->rli.data_lock);

  m_row.channel_name_length= static_cast<uint>(mi->connection_name.length);
  memcpy(m_row.channel_name, mi->connection_name.str, m_row.channel_name_length);

  if (mi->rli.slave_running)
  {
    PSI_thread *psi= thd_get_psi(mi->rli.sql_driver_thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
    else
      m_row.thread_id_is_null= true;
  }
  else
    m_row.thread_id_is_null= true;

  if (mi->rli.slave_running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  mysql_mutex_lock(&mi->rli.err_lock);

  m_row.last_error_number= (long int) mi->rli.last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char *temp_store= (char*) mi->rli.last_error().message;
    m_row.last_error_message_length= static_cast<uint>(strlen(temp_store));
    memcpy(m_row.last_error_message, temp_store,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= 0;//(ulonglong)mi->rli.last_error().skr*1000000;
  }

  mysql_mutex_unlock(&mi->rli.err_lock);
  mysql_mutex_unlock(&mi->rli.data_lock);

  m_row_exists= true;
}

int table_replication_applier_status_by_coordinator
  ::read_row_values(TABLE *table, unsigned char *buf,
                    Field **fields, bool read_all)
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
      case 0: /* channel_name */
         set_field_char_utf8(f, m_row.channel_name, m_row.channel_name_length);
         break;
      case 1: /*thread_id*/
        if (!m_row.thread_id_is_null)
          set_field_ulonglong(f, m_row.thread_id);
        else
          f->set_null();
        break;
      case 2: /*service_state*/
        set_field_enum(f, m_row.service_state);
        break;
      case 3: /*last_error_number*/
        set_field_ulong(f, m_row.last_error_number);
        break;
      case 4: /*last_error_message*/
        set_field_varchar_utf8(f, m_row.last_error_message,
                               m_row.last_error_message_length);
        break;
      case 5: /*last_error_timestamp*/
        set_field_timestamp(f, m_row.last_error_timestamp);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}
#endif
