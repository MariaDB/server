/* Copyright (c) 2008, 2014, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "my_global.h"
#include "my_pthread.h"
#include "table_threads.h"
#include "sql_parse.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"

THR_LOCK table_threads::m_table_lock;

PFS_engine_table_share
table_threads::m_share=
{
  { C_STRING_WITH_LEN("threads") },
  &pfs_updatable_acl,
  &table_threads::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE threads("
                      "THREAD_ID BIGINT unsigned not null,"
                      "NAME VARCHAR(128) not null,"
                      "TYPE VARCHAR(10) not null,"
                      "PROCESSLIST_ID BIGINT unsigned,"
                      "PROCESSLIST_USER VARCHAR(16),"
                      "PROCESSLIST_HOST VARCHAR(60),"
                      "PROCESSLIST_DB VARCHAR(64),"
                      "PROCESSLIST_COMMAND VARCHAR(16),"
                      "PROCESSLIST_TIME BIGINT,"
                      "PROCESSLIST_STATE VARCHAR(64),"
                      "PROCESSLIST_INFO LONGTEXT,"
                      "PARENT_THREAD_ID BIGINT unsigned,"
                      "ROLE VARCHAR(64),"
                      "INSTRUMENTED ENUM ('YES', 'NO') not null)") }
};

PFS_engine_table* table_threads::create()
{
  return new table_threads();
}

table_threads::table_threads()
  : cursor_by_thread(& m_share),
  m_row_exists(false)
{}

void table_threads::make_row(PFS_thread *pfs)
{
  pfs_lock lock;
  pfs_lock session_lock;
  pfs_lock stmt_lock;
  PFS_stage_class *stage_class;
  PFS_thread_class *safe_class;

  m_row_exists= false;

  /* Protect this reader against thread termination */
  pfs->m_lock.begin_optimistic_lock(&lock);

  safe_class= sanitize_thread_class(pfs->m_class);
  if (unlikely(safe_class == NULL))
    return;

  m_row.m_thread_internal_id= pfs->m_thread_internal_id;
  m_row.m_parent_thread_internal_id= pfs->m_parent_thread_internal_id;
  m_row.m_processlist_id= pfs->m_processlist_id;
  m_row.m_name= safe_class->m_name;
  m_row.m_name_length= safe_class->m_name_length;

  /* Protect this reader against session attribute changes */
  pfs->m_session_lock.begin_optimistic_lock(&session_lock);

  m_row.m_username_length= pfs->m_username_length;
  if (unlikely(m_row.m_username_length > sizeof(m_row.m_username)))
    return;
  if (m_row.m_username_length != 0)
    memcpy(m_row.m_username, pfs->m_username, m_row.m_username_length);

  m_row.m_hostname_length= pfs->m_hostname_length;
  if (unlikely(m_row.m_hostname_length > sizeof(m_row.m_hostname)))
    return;
  if (m_row.m_hostname_length != 0)
    memcpy(m_row.m_hostname, pfs->m_hostname, m_row.m_hostname_length);

  if (! pfs->m_session_lock.end_optimistic_lock(& session_lock))
  {
    /*
      One of the columns:
      - PROCESSLIST_USER
      - PROCESSLIST_HOST
      is being updated.
      Do not discard the entire row.
      Do not loop waiting for a stable value.
      Just return NULL values.
    */
    m_row.m_username_length= 0;
    m_row.m_hostname_length= 0;
  }

  /* Protect this reader against statement attributes changes */
  pfs->m_stmt_lock.begin_optimistic_lock(&stmt_lock);

  m_row.m_dbname_length= pfs->m_dbname_length;
  if (unlikely(m_row.m_dbname_length > sizeof(m_row.m_dbname)))
    return;
  if (m_row.m_dbname_length != 0)
    memcpy(m_row.m_dbname, pfs->m_dbname, m_row.m_dbname_length);

  m_row.m_processlist_info_ptr= & pfs->m_processlist_info[0];
  m_row.m_processlist_info_length= pfs->m_processlist_info_length;

  if (! pfs->m_stmt_lock.end_optimistic_lock(& stmt_lock))
  {
    /*
      One of the columns:
      - PROCESSLIST_DB
      - PROCESSLIST_INFO
      is being updated.
      Do not discard the entire row.
      Do not loop waiting for a stable value.
      Just return NULL values.
    */
    m_row.m_dbname_length= 0;
    m_row.m_processlist_info_length= 0;
  }

  /* Dirty read, sanitize the command. */
  m_row.m_command= pfs->m_command;
  if ((m_row.m_command < 0) || (m_row.m_command > COM_END))
    m_row.m_command= COM_END;

  m_row.m_start_time= pfs->m_start_time;

  stage_class= find_stage_class(pfs->m_stage);
  if (stage_class != NULL)
  {
    m_row.m_processlist_state_ptr= stage_class->m_name + stage_class->m_prefix_length;
    m_row.m_processlist_state_length= stage_class->m_name_length - stage_class->m_prefix_length;
  }
  else
  {
    m_row.m_processlist_state_length= 0;
  }

  m_row.m_enabled_ptr= &pfs->m_enabled;

  if (pfs->m_lock.end_optimistic_lock(& lock))
    m_row_exists= true;
}

int table_threads::read_row_values(TABLE *table,
                                   unsigned char *buf,
                                   Field **fields,
                                   bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 2);
  buf[0]= 0;
  buf[1]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* THREAD_ID */
        set_field_ulonglong(f, m_row.m_thread_internal_id);
        break;
      case 1: /* NAME */
        set_field_varchar_utf8(f, m_row.m_name, m_row.m_name_length);
        break;
      case 2: /* TYPE */
        if (m_row.m_processlist_id != 0)
          set_field_varchar_utf8(f, "FOREGROUND", 10);
        else
          set_field_varchar_utf8(f, "BACKGROUND", 10);
        break;
      case 3: /* PROCESSLIST_ID */
        if (m_row.m_processlist_id != 0)
          set_field_ulonglong(f, m_row.m_processlist_id);
        else
          f->set_null();
        break;
      case 4: /* PROCESSLIST_USER */
        if (m_row.m_username_length > 0)
          set_field_varchar_utf8(f, m_row.m_username,
                                 m_row.m_username_length);
        else
          f->set_null();
        break;
      case 5: /* PROCESSLIST_HOST */
        if (m_row.m_hostname_length > 0)
          set_field_varchar_utf8(f, m_row.m_hostname,
                                 m_row.m_hostname_length);
        else
          f->set_null();
        break;
      case 6: /* PROCESSLIST_DB */
        if (m_row.m_dbname_length > 0)
          set_field_varchar_utf8(f, m_row.m_dbname,
                                 m_row.m_dbname_length);
        else
          f->set_null();
        break;
      case 7: /* PROCESSLIST_COMMAND */
        if (m_row.m_processlist_id != 0)
          set_field_varchar_utf8(f, command_name[m_row.m_command].str,
                                 command_name[m_row.m_command].length);
        else
          f->set_null();
        break;
      case 8: /* PROCESSLIST_TIME */
        if (m_row.m_start_time)
        {
          time_t now= my_time(0);
          ulonglong elapsed= (now > m_row.m_start_time ? now - m_row.m_start_time : 0);
          set_field_ulonglong(f, elapsed);
        }
        else
          f->set_null();
        break;
      case 9: /* PROCESSLIST_STATE */
        if (m_row.m_processlist_state_length > 0)
        {
          /* This column's datatype is declared as varchar(64). But in current
             code, there are few process state messages which are greater than
             64 characters(Eg:stage_slave_has_read_all_relay_log).
             In those cases, we will end up in 'data truncated'
             warning/error (depends sql_mode setting) when server is updating
             this column for those threads. Since 5.6 is GAed, neither the
             metadata of this column can be changed, nor those state messages.
             So server will silently truncate the state message to 64 characters
             if it is longer. In Upper versions(5.7+), these state messages are
             changed to less than or equal to 64 characters.
           */
          set_field_varchar_utf8(f, m_row.m_processlist_state_ptr,
                                 std::min<uint>(m_row.m_processlist_state_length,
                                                f->char_length()));
        }
        else
          f->set_null();
        break;
      case 10: /* PROCESSLIST_INFO */
        if (m_row.m_processlist_info_length > 0)
          set_field_longtext_utf8(f, m_row.m_processlist_info_ptr,
                                  m_row.m_processlist_info_length);
        else
          f->set_null();
        break;
      case 11: /* PARENT_THREAD_ID */
        if (m_row.m_parent_thread_internal_id != 0)
          set_field_ulonglong(f, m_row.m_parent_thread_internal_id);
        else
          f->set_null();
        break;
      case 12: /* ROLE */
        f->set_null();
        break;
      case 13: /* INSTRUMENTED */
        set_field_enum(f, (*m_row.m_enabled_ptr) ? ENUM_YES : ENUM_NO);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}

int table_threads::update_row_values(TABLE *table,
                                     const unsigned char *old_buf,
                                     const unsigned char *new_buf,
                                     Field **fields)
{
  Field *f;
  enum_yes_no value;

  for (; (f= *fields) ; fields++)
  {
    if (bitmap_is_set(table->write_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* THREAD_ID */
      case 1: /* NAME */
      case 2: /* TYPE */
      case 3: /* PROCESSLIST_ID */
      case 4: /* PROCESSLIST_USER */
      case 5: /* PROCESSLIST_HOST */
      case 6: /* PROCESSLIST_DB */
      case 7: /* PROCESSLIST_COMMAND */
      case 8: /* PROCESSLIST_TIME */
      case 9: /* PROCESSLIST_STATE */
      case 10: /* PROCESSLIST_INFO */
      case 11: /* PARENT_THREAD_ID */
      case 12: /* ROLE */
        return HA_ERR_WRONG_COMMAND;
      case 13: /* INSTRUMENTED */
        value= (enum_yes_no) get_field_enum(f);
        *m_row.m_enabled_ptr= (value == ENUM_YES) ? true : false;
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}

