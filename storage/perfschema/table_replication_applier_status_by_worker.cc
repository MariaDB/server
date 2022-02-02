/*
      Copyright (c) 2013, 2021, Oracle and/or its affiliates.

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
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
#include "rpl_parallel.h"

THR_LOCK table_replication_applier_status_by_worker::m_table_lock;

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
  "CHANNEL_NAME VARCHAR(256) collate utf8_general_ci not null comment 'Name of replication channel through which the transaction is received.',"
  "THREAD_ID BIGINT UNSIGNED comment 'Thread_Id as displayed in the performance_schema.threads table for thread with name ''thread/sql/rpl_parallel_thread''. THREAD_ID will be NULL when worker threads are stopped due to error/force stop.',"
  "SERVICE_STATE ENUM('ON','OFF') not null comment 'Whether or not the thread is running.',"
  "LAST_SEEN_TRANSACTION CHAR(57) not null comment 'Last GTID executed by worker',"
  "LAST_ERROR_NUMBER INTEGER not null comment 'Last Error that occurred on a particular worker.',"
  "LAST_ERROR_MESSAGE VARCHAR(1024) not null comment 'Last error specific message.',"
  "LAST_ERROR_TIMESTAMP TIMESTAMP(0) not null comment 'Time stamp of last error.',"
  "WORKER_IDLE_TIME BIGINT UNSIGNED not null comment 'Total idle time in seconds that the worker thread has spent waiting for work from SQL thread.',"
  "LAST_TRANS_RETRY_COUNT INTEGER not null comment 'Total number of retries attempted by last transaction.')") },
  false  /* perpetual */
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
  return opt_slave_parallel_threads;
}

int table_replication_applier_status_by_worker::rnd_next(void)
{
  rpl_parallel_thread_pool *pool= &global_rpl_thread_pool;
  if (pool->inited && pool->count)
  {
    mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
    uint worker_count= pool->count;
    for (m_pos.set_at(&m_next_pos);
        m_pos.has_more_workers(worker_count);
        m_pos.next_worker())
    {
      rpl_parallel_thread *rpt= pool->threads[m_pos.m_index];
      make_row(rpt);
      m_next_pos.set_after(&m_pos);
      mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
      return 0;
    }
    mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
  }
  else
  {
    mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
    struct pool_bkp_for_pfs *bkp_pool= &pool->pfs_bkp;
    if (bkp_pool->inited && bkp_pool->count)
    {
      for (m_pos.set_at(&m_next_pos);
           m_pos.has_more_workers(bkp_pool->count);
           m_pos.next_worker())
      {
        rpl_parallel_thread *rpt= bkp_pool->rpl_thread_arr[m_pos.m_index];
        make_row(rpt);
        m_next_pos.set_after(&m_pos);
        mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
        return 0;
      }
    }
    mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
  }
  return HA_ERR_END_OF_FILE;
}

int table_replication_applier_status_by_worker::rnd_pos(const void *pos)
{
  int res= HA_ERR_RECORD_DELETED;

  set_position(pos);

  if (global_rpl_thread_pool.inited && global_rpl_thread_pool.count)
  {
    rpl_parallel_thread_pool *pool= &global_rpl_thread_pool;
    mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
    if(m_pos.m_index < pool->count)
    {
      rpl_parallel_thread *rpt= pool->threads[m_pos.m_index];
      make_row(rpt);
      mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
      res= 0;
    }
  }
  else
  {
    struct pool_bkp_for_pfs *bkp_pool= &global_rpl_thread_pool.pfs_bkp;
    if (bkp_pool->inited && bkp_pool->count && m_pos.m_index < bkp_pool->count)
    {
      rpl_parallel_thread *rpt= bkp_pool->rpl_thread_arr[m_pos.m_index];
      make_row(rpt);
      res= 0;
    }
  }
  return res;
}

/**
  Function to display slave worker thread specific information

   @param[in] rpl_parallel_thread

   @retval void
*/
void table_replication_applier_status_by_worker::make_row(rpl_parallel_thread *rpt)
{
  char buf[10+1+10+1+20+1];
  String str(buf, sizeof(buf), system_charset_info);
  bool first= true;

  str.length(0);
  rpl_gtid gtid= rpt->last_seen_gtid;

  m_row_exists= false;

  m_row.channel_name_length= rpt->channel_name_length;
  if (m_row.channel_name_length)
    memcpy(m_row.channel_name, rpt->channel_name, m_row.channel_name_length);

  m_row.thread_id_is_null= true;
  if (rpt->running)
  {
    PSI_thread *psi= thd_get_psi(rpt->thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
  }

  if ((gtid.seq_no > 0 &&
       !rpl_slave_state_tostring_helper(&str, &gtid, &first)))
  {
    strmake(m_row.last_seen_transaction,str.ptr(), str.length());
    m_row.last_seen_transaction_length= str.length();
  }
  else
  {
    m_row.last_seen_transaction_length= 0;
    memcpy(m_row.last_seen_transaction, "", 1);
  }

  if (rpt->running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;
  m_row.last_error_number= rpt->last_error_number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;
  if (m_row.last_error_number)
  {
    char* temp_store= (char*)rpt->last_error_message;
    m_row.last_error_message_length= (uint)strlen(temp_store);
    strmake(m_row.last_error_message, rpt->last_error_message,
        m_row.last_error_message_length);
      /** time in millisecond since epoch */
    m_row.last_error_timestamp= rpt->last_error_timestamp;
  }

  m_row.last_trans_retry_count= rpt->last_trans_retry_count;
  m_row.worker_idle_time= rpt->get_worker_idle_time();
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
      case 0: /*channel_name*/
        set_field_varchar_utf8(f, m_row.channel_name, m_row.channel_name_length);
        break;
      case 1: /*thread_id*/
        if(m_row.thread_id_is_null)
          f->set_null();
        else
          set_field_ulonglong(f, m_row.thread_id);
        break;
      case 2: /*service_state*/
        set_field_enum(f, m_row.service_state);
        break;
      case 3: /*last_seen_transaction*/
        set_field_char_utf8(f, m_row.last_seen_transaction, m_row.last_seen_transaction_length);
        break;
      case 4: /*last_error_number*/
        set_field_ulong(f, m_row.last_error_number);
        break;
      case 5: /*last_error_message*/
        set_field_varchar_utf8(f, m_row.last_error_message, m_row.last_error_message_length);
        break;
      case 6: /*last_error_timestamp*/
        set_field_timestamp(f, m_row.last_error_timestamp);
        break;
      case 7: /*worker_idle_time*/
        set_field_ulonglong(f, m_row.worker_idle_time);
        break;
      case 8: /*last_trans_retry_count*/
        set_field_ulong(f, m_row.last_trans_retry_count);
        break;
      default:
        assert(false);
      }
    }
  }
  return 0;
}
