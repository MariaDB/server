/*
      Copyright (c) 2013, 2023, Oracle and/or its affiliates.

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
  @file storage/perfschema/table_replication_applier_status_by_worker.cc
  Table replication_applier_status_by_worker (implementation).
*/

//#define HAVE_REPLICATION

#include "my_global.h"
#include "table_replication_applier_status_by_worker.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "slave.h"
//#include "rpl_info.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
//#include "rpl_rli_pdb.h"
//#include "rpl_msr.h"    /*Multi source replication */

THR_LOCK table_replication_applier_status_by_worker::m_table_lock;

PFS_engine_table_share_state
table_replication_applier_status_by_worker::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_replication_applier_status_by_worker::m_share=
{
  { C_STRING_WITH_LEN("replication_applier_status_by_worker") },
  &pfs_readonly_acl,
  table_replication_applier_status_by_worker::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_applier_status_by_worker::get_row_count, /*records*/
  sizeof(pos_t), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE replication_applier_status_by_worker("
  "CHANNEL_NAME CHAR(64) collate utf8_general_ci not null comment 'Name of replication channel through which the transaction is received.',"
  "WORKER_ID BIGINT UNSIGNED not null comment 'Worker identifier.,"
  "THREAD_ID BIGINT UNSIGNED comment 'Thread_Id as displayed in the performance_schema.threads table for thread with name ''thread/sql/rpl_parallel_thread''. THREAD_ID will be NULL when worker threads are stopped due to error/force stop.',"
  "SERVICE_STATE ENUM('ON','OFF') not null comment 'Whether or not the thread is running.',"
  "LAST_SEEN_TRANSACTION CHAR(57) not null comment 'Last GTID executed by worker',"
  "LAST_ERROR_NUMBER INTEGER not null comment 'Last Error that occurred on a particular worker.',"
  "LAST_ERROR_MESSAGE VARCHAR(1024) not null comment 'Last error specific message.',"
  "LAST_ERROR_TIMESTAMP TIMESTAMP(0) not null comment 'Time stamp of last error.')") },
  false, /* m_perpetual */
  &m_share_state
};

PFS_engine_table* table_replication_applier_status_by_worker::create(void)
{
  return new table_replication_applier_status_by_worker();
}

table_replication_applier_status_by_worker
  ::table_replication_applier_status_by_worker()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(), m_next_pos()
{}

table_replication_applier_status_by_worker
  ::~table_replication_applier_status_by_worker()
{}

void table_replication_applier_status_by_worker::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

ha_rows table_replication_applier_status_by_worker::get_row_count()
{
  /*
    Return an estimate, number of master info's multipled by worker threads
  */
 return  master_info_index->master_info_hash.records*32;
}


int table_replication_applier_status_by_worker::rnd_next(void)
{
  Slave_worker *worker;
  Master_info *mi;
  size_t wc;

  mysql_mutex_lock(&LOCK_active_mi);

  for (m_pos.set_at(&m_next_pos);
       m_pos.has_more_channels(master_info_index->master_info_hash.records);
       m_pos.next_channel())
  {
    mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index_1);

    if (mi && mi->host[0])
    {
      wc= mi->rli->get_worker_count();

      if (wc == 0)
      {
        /* Single Thread Slave */
        make_row(mi);
        m_next_pos.set_channel_after(&m_pos);
        channel_map.unlock();
        return 0;
      }

      for (; m_pos.m_index_2 < wc; m_pos.next_worker())
      {
        /* Multi Thread Slave */

        worker = mi->rli->get_worker(m_pos.m_index_2);
        if (worker)
        {
          make_row(worker);
          m_next_pos.set_after(&m_pos);
          channel_map.unlock();
          return 0;
        }
       }
    }
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  return HA_ERR_END_OF_FILE;
}

int table_replication_applier_status_by_worker::rnd_pos(const void *pos)
{
  Slave_worker *worker;
  Master_info *mi;
  int res= HA_ERR_RECORD_DELETED;
  size_t wc;

  set_position(pos);

  mysql_mutex_lock(&LOCK_active_mi);

  mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index_1);

  if (!mi || !mi->host[0])
    goto end;

  wc = mi->rli->get_worker_count();

  if (wc == 0)
  {
    /* Single Thread Slave */
    make_row(mi);
    res=0;
  }
  else
  {
    /* Multi Thread Slave */
    if (m_pos.m_index_2 < wc)
    {
      worker = mi->rli->get_worker(m_pos.m_index_2);
      if (worker != NULL)
      {
        make_row(worker);
        res=0;
      }
    }
  }

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

/**
   Function to display SQL Thread's status as part of
   'replication_applier_status_by_worker' in single threaded slave mode.

   @param[in] Master_info

   @retval void
*/
void table_replication_applier_status_by_worker::make_row(Master_info *mi)
{
  m_row_exists= false;

  m_row.worker_id= 0;

  m_row.thread_id= 0;

  assert(mi != NULL);
  assert(mi->rli != NULL);

  mysql_mutex_lock(&mi->rli->data_lock);

  m_row.channel_name_length= strlen(mi->get_channel());
  memcpy(m_row.channel_name, (char*)mi->get_channel(), m_row.channel_name_length);

  if (mi->rli->slave_running)
  {
    PSI_thread *psi= thd_get_psi(mi->rli->info_thd);
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

  if (mi->rli->slave_running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  if (mi->rli->currently_executing_gtid.type == GTID_GROUP)
  {
    global_sid_lock->rdlock();
    m_row.last_seen_transaction_length=
      mi->rli->currently_executing_gtid.to_string(global_sid_map,
                                            m_row.last_seen_transaction);
    global_sid_lock->unlock();
  }
  else if (mi->rli->currently_executing_gtid.type == ANONYMOUS_GROUP)
  {
    m_row.last_seen_transaction_length=
      mi->rli->currently_executing_gtid.to_string((rpl_sid *)NULL,
                                            m_row.last_seen_transaction);
  }
  else
  {
    /*
      For SQL thread currently_executing_gtid, type is set to
      AUTOMATIC_GROUP when the SQL thread is not executing any
      transaction.  For this case, the field should be empty.
    */
    assert(mi->rli->currently_executing_gtid.type == AUTOMATIC_GROUP);
    m_row.last_seen_transaction_length= 0;
    memcpy(m_row.last_seen_transaction, "", 1);
  }

  mysql_mutex_lock(&mi->rli->err_lock);

  m_row.last_error_number= (long int) mi->rli->last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char *temp_store= (char*) mi->rli->last_error().message;
    m_row.last_error_message_length= strlen(temp_store);
    memcpy(m_row.last_error_message, temp_store,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= (ulonglong)mi->rli->last_error().skr*1000000;
  }

  mysql_mutex_unlock(&mi->rli->err_lock);
  mysql_mutex_unlock(&mi->rli->data_lock);
  m_row_exists= true;
}

void table_replication_applier_status_by_worker::make_row(Slave_worker *w)
{
  m_row_exists= false;

  m_row.worker_id= w->get_internal_id();

  m_row.thread_id= 0;

  m_row.channel_name_length= strlen(w->get_channel());
  memcpy(m_row.channel_name, (char*)w->get_channel(), m_row.channel_name_length);

  mysql_mutex_lock(&w->jobs_lock);
  if (w->running_status == Slave_worker::RUNNING)
  {
    PSI_thread *psi= thd_get_psi(w->info_thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
    else /* no instrumentation found */
      m_row.thread_id_is_null= true;
  }
  else
    m_row.thread_id_is_null= true;

  if (w->running_status == Slave_worker::RUNNING)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  m_row.last_error_number= (unsigned int) w->last_error().number;

  if (w->currently_executing_gtid.type == GTID_GROUP)
  {
    global_sid_lock->rdlock();
    m_row.last_seen_transaction_length=
      w->currently_executing_gtid.to_string(global_sid_map,
                                            m_row.last_seen_transaction);
    global_sid_lock->unlock();
  }
  else if (w->currently_executing_gtid.type == ANONYMOUS_GROUP)
  {
    m_row.last_seen_transaction_length=
      w->currently_executing_gtid.to_string((rpl_sid *)NULL,
                                            m_row.last_seen_transaction);
  }
  else
  {
    /*
      For worker->currently_executing_gtid, type is set to
      AUTOMATIC_GROUP when the worker is not executing any
      transaction.  For this case, the field should be empty.
    */
    assert(w->currently_executing_gtid.type == AUTOMATIC_GROUP);
    m_row.last_seen_transaction_length= 0;
    memcpy(m_row.last_seen_transaction, "", 1);
  }

  m_row.last_error_number= (unsigned int) w->last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char * temp_store= (char*)w->last_error().message;
    m_row.last_error_message_length= strlen(temp_store);
    memcpy(m_row.last_error_message, w->last_error().message,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= (ulonglong)w->last_error().skr*1000000;
  }
  mysql_mutex_unlock(&w->jobs_lock);

  m_row_exists= true;
}

int table_replication_applier_status_by_worker
  ::read_row_values(TABLE *table, unsigned char *buf,  Field **fields,
                    bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /** channel_name */
        set_field_char_utf8(f, m_row.channel_name, m_row.channel_name_length);
        break;
      case 1: /*worker_id*/
        set_field_ulonglong(f, m_row.worker_id);
        break;
      case 2: /*thread_id*/
        if(m_row.thread_id_is_null)
          f->set_null();
        else
          set_field_ulonglong(f, m_row.thread_id);
        break;
      case 3: /*service_state*/
        set_field_enum(f, m_row.service_state);
        break;
      case 4: /*last_seen_transaction*/
        set_field_char_utf8(f, m_row.last_seen_transaction, m_row.last_seen_transaction_length);
        break;
      case 5: /*last_error_number*/
        set_field_ulong(f, m_row.last_error_number);
        break;
      case 6: /*last_error_message*/
        set_field_varchar_utf8(f, m_row.last_error_message, m_row.last_error_message_length);
        break;
      case 7: /*last_error_timestamp*/
        set_field_timestamp(f, m_row.last_error_timestamp);
        break;
      default:
        assert(false);
      }
    }
  }
  return 0;
}
