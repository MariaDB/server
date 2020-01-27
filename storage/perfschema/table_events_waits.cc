/* Copyright (c) 2008, 2015, Oracle and/or its affiliates. All rights reserved.

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
  @file storage/perfschema/table_events_waits.cc
  Table EVENTS_WAITS_xxx (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "table_events_waits.h"
#include "pfs_global.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "pfs_events_waits.h"
#include "pfs_timer.h"
#include "m_string.h"

THR_LOCK table_events_waits_current::m_table_lock;

PFS_engine_table_share
table_events_waits_current::m_share=
{
  { C_STRING_WITH_LEN("events_waits_current") },
  &pfs_truncatable_acl,
  &table_events_waits_current::create,
  NULL, /* write_row */
  &table_events_waits_current::delete_all_rows,
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(pos_events_waits_current), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_waits_current("
                      "THREAD_ID BIGINT unsigned not null,"
                      "EVENT_ID BIGINT unsigned not null,"
                      "END_EVENT_ID BIGINT unsigned,"
                      "EVENT_NAME VARCHAR(128) not null,"
                      "SOURCE VARCHAR(64),"
                      "TIMER_START BIGINT unsigned,"
                      "TIMER_END BIGINT unsigned,"
                      "TIMER_WAIT BIGINT unsigned,"
                      "SPINS INTEGER unsigned,"
                      "OBJECT_SCHEMA VARCHAR(64),"
                      "OBJECT_NAME VARCHAR(512),"
                      "INDEX_NAME VARCHAR(64),"
                      "OBJECT_TYPE VARCHAR(64),"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
                      "NESTING_EVENT_ID BIGINT unsigned,"
                      "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
                      "OPERATION VARCHAR(32) not null,"
                      "NUMBER_OF_BYTES BIGINT,"
                      "FLAGS INTEGER unsigned)") }
};

THR_LOCK table_events_waits_history::m_table_lock;

PFS_engine_table_share
table_events_waits_history::m_share=
{
  { C_STRING_WITH_LEN("events_waits_history") },
  &pfs_truncatable_acl,
  &table_events_waits_history::create,
  NULL, /* write_row */
  &table_events_waits_history::delete_all_rows,
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(pos_events_waits_history), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_waits_history("
                      "THREAD_ID BIGINT unsigned not null,"
                      "EVENT_ID BIGINT unsigned not null,"
                      "END_EVENT_ID BIGINT unsigned,"
                      "EVENT_NAME VARCHAR(128) not null,"
                      "SOURCE VARCHAR(64),"
                      "TIMER_START BIGINT unsigned,"
                      "TIMER_END BIGINT unsigned,"
                      "TIMER_WAIT BIGINT unsigned,"
                      "SPINS INTEGER unsigned,"
                      "OBJECT_SCHEMA VARCHAR(64),"
                      "OBJECT_NAME VARCHAR(512),"
                      "INDEX_NAME VARCHAR(64),"
                      "OBJECT_TYPE VARCHAR(64),"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
                      "NESTING_EVENT_ID BIGINT unsigned,"
                      "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
                      "OPERATION VARCHAR(32) not null,"
                      "NUMBER_OF_BYTES BIGINT,"
                      "FLAGS INTEGER unsigned)") }
};

THR_LOCK table_events_waits_history_long::m_table_lock;

PFS_engine_table_share
table_events_waits_history_long::m_share=
{
  { C_STRING_WITH_LEN("events_waits_history_long") },
  &pfs_truncatable_acl,
  &table_events_waits_history_long::create,
  NULL, /* write_row */
  &table_events_waits_history_long::delete_all_rows,
  NULL, /* get_row_count */
  10000, /* records */
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_waits_history_long("
                      "THREAD_ID BIGINT unsigned not null,"
                      "EVENT_ID BIGINT unsigned not null,"
                      "END_EVENT_ID BIGINT unsigned,"
                      "EVENT_NAME VARCHAR(128) not null,"
                      "SOURCE VARCHAR(64),"
                      "TIMER_START BIGINT unsigned,"
                      "TIMER_END BIGINT unsigned,"
                      "TIMER_WAIT BIGINT unsigned,"
                      "SPINS INTEGER unsigned,"
                      "OBJECT_SCHEMA VARCHAR(64),"
                      "OBJECT_NAME VARCHAR(512),"
                      "INDEX_NAME VARCHAR(64),"
                      "OBJECT_TYPE VARCHAR(64),"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
                      "NESTING_EVENT_ID BIGINT unsigned,"
                      "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
                      "OPERATION VARCHAR(32) not null,"
                      "NUMBER_OF_BYTES BIGINT,"
                      "FLAGS INTEGER unsigned)") }
};

table_events_waits_common::table_events_waits_common
(const PFS_engine_table_share *share, void *pos)
  : PFS_engine_table(share, pos),
  m_row_exists(false)
{}

void table_events_waits_common::clear_object_columns()
{
  m_row.m_object_type= NULL;
  m_row.m_object_type_length= 0;
  m_row.m_object_schema_length= 0;
  m_row.m_object_name_length= 0;
  m_row.m_index_name_length= 0;
  m_row.m_object_instance_addr= 0;
}

int table_events_waits_common::make_table_object_columns(volatile PFS_events_waits *wait)
{
  uint safe_index;
  PFS_table_share *safe_table_share;

  safe_table_share= sanitize_table_share(wait->m_weak_table_share);
  if (unlikely(safe_table_share == NULL))
    return 1;

  if (wait->m_object_type == OBJECT_TYPE_TABLE)
  {
    m_row.m_object_type= "TABLE";
    m_row.m_object_type_length= 5;
  }
  else
  {
    m_row.m_object_type= "TEMPORARY TABLE";
    m_row.m_object_type_length= 15;
  }

  if (safe_table_share->get_version() == wait->m_weak_version)
  {
    /* OBJECT SCHEMA */
    m_row.m_object_schema_length= safe_table_share->m_schema_name_length;
    if (unlikely((m_row.m_object_schema_length == 0) ||
                 (m_row.m_object_schema_length > sizeof(m_row.m_object_schema))))
      return 1;
    memcpy(m_row.m_object_schema, safe_table_share->m_schema_name, m_row.m_object_schema_length);

    /* OBJECT NAME */
    m_row.m_object_name_length= safe_table_share->m_table_name_length;
    if (unlikely((m_row.m_object_name_length == 0) ||
                 (m_row.m_object_name_length > sizeof(m_row.m_object_name))))
      return 1;
    memcpy(m_row.m_object_name, safe_table_share->m_table_name, m_row.m_object_name_length);

    /* INDEX NAME */
    safe_index= wait->m_index;
    uint safe_key_count= sanitize_index_count(safe_table_share->m_key_count);
    if (safe_index < safe_key_count)
    {
      PFS_table_key *key= & safe_table_share->m_keys[safe_index];
      m_row.m_index_name_length= key->m_name_length;
      if (unlikely((m_row.m_index_name_length == 0) ||
                   (m_row.m_index_name_length > sizeof(m_row.m_index_name))))
        return 1;
      memcpy(m_row.m_index_name, key->m_name, m_row.m_index_name_length);
    }
    else
      m_row.m_index_name_length= 0;
  }
  else
  {
    m_row.m_object_schema_length= 0;
    m_row.m_object_name_length= 0;
    m_row.m_index_name_length= 0;
  }

  m_row.m_object_instance_addr= (intptr) wait->m_object_instance_addr;
  return 0;
}

int table_events_waits_common::make_file_object_columns(volatile PFS_events_waits *wait)
{
  PFS_file *safe_file;

  safe_file= sanitize_file(wait->m_weak_file);
  if (unlikely(safe_file == NULL))
    return 1;

  m_row.m_object_type= "FILE";
  m_row.m_object_type_length= 4;
  m_row.m_object_schema_length= 0;
  m_row.m_object_instance_addr= (intptr) wait->m_object_instance_addr;

  if (safe_file->get_version() == wait->m_weak_version)
  {
    /* OBJECT NAME */
    m_row.m_object_name_length= safe_file->m_filename_length;
    if (unlikely((m_row.m_object_name_length == 0) ||
                 (m_row.m_object_name_length > sizeof(m_row.m_object_name))))
      return 1;
    memcpy(m_row.m_object_name, safe_file->m_filename, m_row.m_object_name_length);
  }
  else
  {
    m_row.m_object_name_length= 0;
  }

  m_row.m_index_name_length= 0;

  return 0;
}

int table_events_waits_common::make_socket_object_columns(volatile PFS_events_waits *wait)
{
  PFS_socket *safe_socket;

  safe_socket= sanitize_socket(wait->m_weak_socket);
  if (unlikely(safe_socket == NULL))
    return 1;

  m_row.m_object_type= "SOCKET";
  m_row.m_object_type_length= 6;
  m_row.m_object_schema_length= 0;
  m_row.m_object_instance_addr= (intptr) wait->m_object_instance_addr;

  if (safe_socket->get_version() == wait->m_weak_version)
  {
    /* Convert port number to string, include delimiter in port name length */

    uint port;
    char port_str[128];
    char ip_str[INET6_ADDRSTRLEN+1];
    /*
      "ip_length" was "ip_len" originally.
      but it conflicted with some macro on AIX. Renamed.
    */
    uint ip_length= 0;
    port_str[0]= ':';

    /* Get the IP address and port number */
    ip_length= pfs_get_socket_address(ip_str, sizeof(ip_str), &port,
                                   &safe_socket->m_sock_addr,
                                   safe_socket->m_addr_len);

    /* Convert port number to a string (length includes ':') */
    int port_len= (int)(int10_to_str(port, (port_str+1), 10) - port_str + 1);

    /* OBJECT NAME */
    m_row.m_object_name_length= ip_length + port_len;

    if (unlikely((m_row.m_object_name_length == 0) ||
                 (m_row.m_object_name_length > sizeof(m_row.m_object_name))))
      return 1;

    char *name= m_row.m_object_name;
    memcpy(name, ip_str, ip_length);
    memcpy(name + ip_length, port_str, port_len);
  }
  else
  {
    m_row.m_object_name_length= 0;
  }

  m_row.m_index_name_length= 0;

  return 0;
}

/**
  Build a row.
  @param thread_own_wait            True if the memory for the wait
    is owned by pfs_thread
  @param pfs_thread                 the thread the cursor is reading
  @param wait                       the wait the cursor is reading
*/
void table_events_waits_common::make_row(bool thread_own_wait,
                                         PFS_thread *pfs_thread,
                                         volatile PFS_events_waits *wait)
{
  pfs_lock lock;
  PFS_thread *safe_thread;
  PFS_instr_class *safe_class;
  const char *base;
  const char *safe_source_file;
  enum_timer_name timer_name= wait_timer;
  ulonglong timer_end;

  m_row_exists= false;
  safe_thread= sanitize_thread(pfs_thread);
  if (unlikely(safe_thread == NULL))
    return;

  /* Protect this reader against a thread termination */
  if (thread_own_wait)
    safe_thread->m_lock.begin_optimistic_lock(&lock);

  /*
    Design choice:
    We could have used a pfs_lock in PFS_events_waits here,
    to protect the reader from concurrent event generation,
    but this leads to too many pfs_lock atomic operations
    each time an event is recorded:
    - 1 dirty() + 1 allocated() per event start, for EVENTS_WAITS_CURRENT
    - 1 dirty() + 1 allocated() per event end, for EVENTS_WAITS_CURRENT
    - 1 dirty() + 1 allocated() per copy to EVENTS_WAITS_HISTORY
    - 1 dirty() + 1 allocated() per copy to EVENTS_WAITS_HISTORY_LONG
    or 8 atomics per recorded event.
    The problem is that we record a *lot* of events ...

    This code is prepared to accept *dirty* records,
    and sanitizes all the data before returning a row.
  */

  /*
    PFS_events_waits::m_class needs to be sanitized,
    for race conditions when this code:
    - reads a new value in m_wait_class,
    - reads an old value in m_class.
  */
  switch (wait->m_wait_class)
  {
  case WAIT_CLASS_IDLE:
    clear_object_columns();
    safe_class= sanitize_idle_class(wait->m_class);
    timer_name= idle_timer;
    break;
  case WAIT_CLASS_MUTEX:
    clear_object_columns();
    safe_class= sanitize_mutex_class((PFS_mutex_class*) wait->m_class);
    break;
  case WAIT_CLASS_RWLOCK:
    clear_object_columns();
    safe_class= sanitize_rwlock_class((PFS_rwlock_class*) wait->m_class);
    break;
  case WAIT_CLASS_COND:
    clear_object_columns();
    safe_class= sanitize_cond_class((PFS_cond_class*) wait->m_class);
    break;
  case WAIT_CLASS_TABLE:
    if (make_table_object_columns(wait))
      return;
    safe_class= sanitize_table_class(wait->m_class);
    break;
  case WAIT_CLASS_FILE:
    if (make_file_object_columns(wait))
      return;
    safe_class= sanitize_file_class((PFS_file_class*) wait->m_class);
    break;
  case WAIT_CLASS_SOCKET:
    if (make_socket_object_columns(wait))
      return;
    safe_class= sanitize_socket_class((PFS_socket_class*) wait->m_class);
    break;
  case NO_WAIT_CLASS:
  default:
    return;
  }

  if (unlikely(safe_class == NULL))
    return;

  m_row.m_thread_internal_id= safe_thread->m_thread_internal_id;
  m_row.m_event_id= wait->m_event_id;
  m_row.m_end_event_id= wait->m_end_event_id;
  m_row.m_nesting_event_id= wait->m_nesting_event_id;
  m_row.m_nesting_event_type= wait->m_nesting_event_type;

  get_normalizer(safe_class);

  if (m_row.m_end_event_id == 0)
  {
    timer_end= get_timer_raw_value(timer_name);
  }
  else
  {
    timer_end= wait->m_timer_end;
  }

  m_normalizer->to_pico(wait->m_timer_start, timer_end,
                      & m_row.m_timer_start, & m_row.m_timer_end, & m_row.m_timer_wait);

  m_row.m_name= safe_class->m_name;
  m_row.m_name_length= safe_class->m_name_length;

  /*
    We are assuming this pointer is sane,
    since it comes from __FILE__.
  */
  safe_source_file= wait->m_source_file;
  if (unlikely(safe_source_file == NULL))
    return;

  base= base_name(wait->m_source_file);
  m_row.m_source_length= (uint)my_snprintf(m_row.m_source, sizeof(m_row.m_source),
                                     "%s:%d", base, wait->m_source_line);
  if (m_row.m_source_length > sizeof(m_row.m_source))
    m_row.m_source_length= sizeof(m_row.m_source);
  m_row.m_operation= wait->m_operation;
  m_row.m_number_of_bytes= wait->m_number_of_bytes;
  m_row.m_flags= wait->m_flags;

  if (thread_own_wait)
  {
    if (safe_thread->m_lock.end_optimistic_lock(&lock))
      m_row_exists= true;
  }
  else
  {
    /*
      For EVENTS_WAITS_HISTORY_LONG (thread_own_wait is false),
      the wait record is always valid, because it is not stored
      in memory owned by pfs_thread.
      Even when the thread terminated, the record is mostly readable,
      so this record is displayed.
    */
    m_row_exists= true;
  }
}

/**
  Operations names map, as displayed in the 'OPERATION' column.
  Indexed by enum_operation_type - 1.
  Note: enum_operation_type contains a more precise definition,
  since more details are needed internally by the instrumentation.
  Different similar operations (CLOSE vs STREAMCLOSE) are displayed
  with the same name 'close'.
*/
static const LEX_STRING operation_names_map[]=
{
  /* Mutex operations */
  { C_STRING_WITH_LEN("lock") },
  { C_STRING_WITH_LEN("try_lock") },

  /* RWLock operations */
  { C_STRING_WITH_LEN("read_lock") },
  { C_STRING_WITH_LEN("write_lock") },
  { C_STRING_WITH_LEN("try_read_lock") },
  { C_STRING_WITH_LEN("try_write_lock") },

  /* Condition operations */
  { C_STRING_WITH_LEN("wait") },
  { C_STRING_WITH_LEN("timed_wait") },

  /* File operations */
  { C_STRING_WITH_LEN("create") },
  { C_STRING_WITH_LEN("create") }, /* create tmp */
  { C_STRING_WITH_LEN("open") },
  { C_STRING_WITH_LEN("open") }, /* stream open */
  { C_STRING_WITH_LEN("close") },
  { C_STRING_WITH_LEN("close") }, /* stream close */
  { C_STRING_WITH_LEN("read") },
  { C_STRING_WITH_LEN("write") },
  { C_STRING_WITH_LEN("seek") },
  { C_STRING_WITH_LEN("tell") },
  { C_STRING_WITH_LEN("flush") },
  { C_STRING_WITH_LEN("stat") },
  { C_STRING_WITH_LEN("stat") }, /* fstat */
  { C_STRING_WITH_LEN("chsize") },
  { C_STRING_WITH_LEN("delete") },
  { C_STRING_WITH_LEN("rename") },
  { C_STRING_WITH_LEN("sync") },

  /* Table io operations */
  { C_STRING_WITH_LEN("fetch") },
  { C_STRING_WITH_LEN("insert") }, /* write row */
  { C_STRING_WITH_LEN("update") }, /* update row */
  { C_STRING_WITH_LEN("delete") }, /* delete row */

  /* Table lock operations */
  { C_STRING_WITH_LEN("read normal") },
  { C_STRING_WITH_LEN("read with shared locks") },
  { C_STRING_WITH_LEN("read high priority") },
  { C_STRING_WITH_LEN("read no inserts") },
  { C_STRING_WITH_LEN("write allow write") },
  { C_STRING_WITH_LEN("write concurrent insert") },
  { C_STRING_WITH_LEN("write delayed") },
  { C_STRING_WITH_LEN("write low priority") },
  { C_STRING_WITH_LEN("write normal") },
  { C_STRING_WITH_LEN("read external") },
  { C_STRING_WITH_LEN("write external") },

  /* Socket operations */
  { C_STRING_WITH_LEN("create") },
  { C_STRING_WITH_LEN("connect") },
  { C_STRING_WITH_LEN("bind") },
  { C_STRING_WITH_LEN("close") },
  { C_STRING_WITH_LEN("send") },
  { C_STRING_WITH_LEN("recv") },
  { C_STRING_WITH_LEN("sendto") },
  { C_STRING_WITH_LEN("recvfrom") },
  { C_STRING_WITH_LEN("sendmsg") },
  { C_STRING_WITH_LEN("recvmsg") },
  { C_STRING_WITH_LEN("seek") },
  { C_STRING_WITH_LEN("opt") },
  { C_STRING_WITH_LEN("stat") },
  { C_STRING_WITH_LEN("shutdown") },
  { C_STRING_WITH_LEN("select") },

  /* Idle operations */
  { C_STRING_WITH_LEN("idle") }
};


int table_events_waits_common::read_row_values(TABLE *table,
                                               unsigned char *buf,
                                               Field **fields,
                                               bool read_all)
{
  Field *f;
  const LEX_STRING *operation;

  compile_time_assert(COUNT_OPERATION_TYPE ==
                      array_elements(operation_names_map));

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 2);
  buf[0]= 0;
  buf[1]= 0;

  /*
    Some columns are unreliable, because they are joined with other buffers,
    which could have changed and been reused for something else.
    These columns are:
    - THREAD_ID (m_thread joins with PFS_thread),
    - SCHEMA_NAME (m_schema_name joins with PFS_table_share)
    - OBJECT_NAME (m_object_name joins with PFS_table_share)
  */
  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* THREAD_ID */
        set_field_ulonglong(f, m_row.m_thread_internal_id);
        break;
      case 1: /* EVENT_ID */
        set_field_ulonglong(f, m_row.m_event_id);
        break;
      case 2: /* END_EVENT_ID */
        if (m_row.m_end_event_id > 0)
          set_field_ulonglong(f, m_row.m_end_event_id - 1);
        else
          f->set_null();
        break;
      case 3: /* EVENT_NAME */
        set_field_varchar_utf8(f, m_row.m_name, m_row.m_name_length);
        break;
      case 4: /* SOURCE */
        set_field_varchar_utf8(f, m_row.m_source, m_row.m_source_length);
        break;
      case 5: /* TIMER_START */
        if (m_row.m_timer_start != 0)
          set_field_ulonglong(f, m_row.m_timer_start);
        else
          f->set_null();
        break;
      case 6: /* TIMER_END */
        if (m_row.m_timer_end != 0)
          set_field_ulonglong(f, m_row.m_timer_end);
        else
          f->set_null();
        break;
      case 7: /* TIMER_WAIT */
        if (m_row.m_timer_wait != 0)
          set_field_ulonglong(f, m_row.m_timer_wait);
        else
          f->set_null();
        break;
      case 8: /* SPINS */
        f->set_null();
        break;
      case 9: /* OBJECT_SCHEMA */
        if (m_row.m_object_schema_length > 0)
        {
          set_field_varchar_utf8(f, m_row.m_object_schema,
                                 m_row.m_object_schema_length);
        }
        else
          f->set_null();
        break;
      case 10: /* OBJECT_NAME */
        if (m_row.m_object_name_length > 0)
        {
          set_field_varchar_utf8(f, m_row.m_object_name,
                                 m_row.m_object_name_length);
        }
        else
          f->set_null();
        break;
      case 11: /* INDEX_NAME */
        if (m_row.m_index_name_length > 0)
        {
          set_field_varchar_utf8(f, m_row.m_index_name,
                                 m_row.m_index_name_length);
        }
        else
          f->set_null();
        break;
      case 12: /* OBJECT_TYPE */
        if (m_row.m_object_type)
        {
          set_field_varchar_utf8(f, m_row.m_object_type,
                                 m_row.m_object_type_length);
        }
        else
          f->set_null();
        break;
      case 13: /* OBJECT_INSTANCE */
        set_field_ulonglong(f, m_row.m_object_instance_addr);
        break;
      case 14: /* NESTING_EVENT_ID */
        if (m_row.m_nesting_event_id != 0)
          set_field_ulonglong(f, m_row.m_nesting_event_id);
        else
          f->set_null();
        break;
      case 15: /* NESTING_EVENT_TYPE */
        if (m_row.m_nesting_event_id != 0)
          set_field_enum(f, m_row.m_nesting_event_type);
        else
          f->set_null();
        break;
      case 16: /* OPERATION */
        operation= &operation_names_map[(int) m_row.m_operation - 1];
        set_field_varchar_utf8(f, operation->str, (uint)operation->length);
        break;
      case 17: /* NUMBER_OF_BYTES */
        if ((m_row.m_operation == OPERATION_TYPE_FILEREAD) ||
            (m_row.m_operation == OPERATION_TYPE_FILEWRITE) ||
            (m_row.m_operation == OPERATION_TYPE_FILECHSIZE) ||
            (m_row.m_operation == OPERATION_TYPE_SOCKETSEND) ||
            (m_row.m_operation == OPERATION_TYPE_SOCKETRECV) ||
            (m_row.m_operation == OPERATION_TYPE_SOCKETSENDTO) ||
            (m_row.m_operation == OPERATION_TYPE_SOCKETRECVFROM))
          set_field_ulonglong(f, m_row.m_number_of_bytes);
        else
          f->set_null();
        break;
      case 18: /* FLAGS */
        f->set_null();
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}

PFS_engine_table* table_events_waits_current::create(void)
{
  return new table_events_waits_current();
}

table_events_waits_current::table_events_waits_current()
  : table_events_waits_common(&m_share, &m_pos),
  m_pos(), m_next_pos()
{}

void table_events_waits_current::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_events_waits_current::rnd_next(void)
{
  PFS_thread *pfs_thread;
  PFS_events_waits *wait;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index_1 < thread_max;
       m_pos.next_thread())
  {
    pfs_thread= &thread_array[m_pos.m_index_1];

    if (! pfs_thread->m_lock.is_populated())
    {
      /* This thread does not exist */
      continue;
    }

    /*
      We do not show nested events for now,
      this will be revised with TABLE io
    */
// #define ONLY_SHOW_ONE_WAIT

#ifdef ONLY_SHOW_ONE_WAIT
    if (m_pos.m_index_2 >= 1)
      continue;
#else
    /* m_events_waits_stack[0] is a dummy record */
    PFS_events_waits *top_wait = &pfs_thread->m_events_waits_stack[WAIT_STACK_BOTTOM];
    wait= &pfs_thread->m_events_waits_stack[m_pos.m_index_2 + WAIT_STACK_BOTTOM];

    PFS_events_waits *safe_current = pfs_thread->m_events_waits_current;

    if (safe_current == top_wait)
    {
      /* Display the last top level wait, when completed */
      if (m_pos.m_index_2 >= 1)
        continue;
    }
    else
    {
      /* Display all pending waits, when in progress */
      if (wait >= safe_current)
        continue;
    }
#endif

    if (wait->m_wait_class == NO_WAIT_CLASS)
    {
      /*
        This locker does not exist.
        There can not be more lockers in the stack, skip to the next thread
      */
      continue;
    }

    make_row(true, pfs_thread, wait);
    /* Next iteration, look for the next locker in this thread */
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_waits_current::rnd_pos(const void *pos)
{
  PFS_thread *pfs_thread;
  PFS_events_waits *wait;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index_1 < thread_max);
  pfs_thread= &thread_array[m_pos.m_index_1];

  if (! pfs_thread->m_lock.is_populated())
    return HA_ERR_RECORD_DELETED;

#ifdef ONLY_SHOW_ONE_WAIT
  if (m_pos.m_index_2 >= 1)
    return HA_ERR_RECORD_DELETED;
#else
  /* m_events_waits_stack[0] is a dummy record */
  PFS_events_waits *top_wait = &pfs_thread->m_events_waits_stack[WAIT_STACK_BOTTOM];
  wait= &pfs_thread->m_events_waits_stack[m_pos.m_index_2 + WAIT_STACK_BOTTOM];

  PFS_events_waits *safe_current = pfs_thread->m_events_waits_current;

  if (safe_current == top_wait)
  {
    /* Display the last top level wait, when completed */
    if (m_pos.m_index_2 >= 1)
      return HA_ERR_RECORD_DELETED;
  }
  else
  {
    /* Display all pending waits, when in progress */
    if (wait >= safe_current)
      return HA_ERR_RECORD_DELETED;
  }
#endif

  DBUG_ASSERT(m_pos.m_index_2 < WAIT_STACK_LOGICAL_SIZE);

  if (wait->m_wait_class == NO_WAIT_CLASS)
    return HA_ERR_RECORD_DELETED;

  make_row(true, pfs_thread, wait);
  return 0;
}

int table_events_waits_current::delete_all_rows(void)
{
  reset_events_waits_current();
  return 0;
}

PFS_engine_table* table_events_waits_history::create(void)
{
  return new table_events_waits_history();
}

table_events_waits_history::table_events_waits_history()
  : table_events_waits_common(&m_share, &m_pos),
  m_pos(), m_next_pos()
{}

void table_events_waits_history::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_events_waits_history::rnd_next(void)
{
  PFS_thread *pfs_thread;
  PFS_events_waits *wait;

  if (events_waits_history_per_thread == 0)
    return HA_ERR_END_OF_FILE;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index_1 < thread_max;
       m_pos.next_thread())
  {
    pfs_thread= &thread_array[m_pos.m_index_1];

    if (! pfs_thread->m_lock.is_populated())
    {
      /* This thread does not exist */
      continue;
    }

    if (m_pos.m_index_2 >= events_waits_history_per_thread)
    {
      /* This thread does not have more (full) history */
      continue;
    }

    if ( ! pfs_thread->m_waits_history_full &&
        (m_pos.m_index_2 >= pfs_thread->m_waits_history_index))
    {
      /* This thread does not have more (not full) history */
      continue;
    }

    if (pfs_thread->m_waits_history[m_pos.m_index_2].m_wait_class
        == NO_WAIT_CLASS)
    {
      /*
        This locker does not exist.
        There can not be more lockers in the stack, skip to the next thread
      */
      continue;
    }

    wait= &pfs_thread->m_waits_history[m_pos.m_index_2];

    make_row(true, pfs_thread, wait);
    /* Next iteration, look for the next history in this thread */
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_waits_history::rnd_pos(const void *pos)
{
  PFS_thread *pfs_thread;
  PFS_events_waits *wait;

  DBUG_ASSERT(events_waits_history_per_thread != 0);
  set_position(pos);
  DBUG_ASSERT(m_pos.m_index_1 < thread_max);
  pfs_thread= &thread_array[m_pos.m_index_1];

  if (! pfs_thread->m_lock.is_populated())
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(m_pos.m_index_2 < events_waits_history_per_thread);

  if ( ! pfs_thread->m_waits_history_full &&
      (m_pos.m_index_2 >= pfs_thread->m_waits_history_index))
    return HA_ERR_RECORD_DELETED;

  wait= &pfs_thread->m_waits_history[m_pos.m_index_2];

  if (wait->m_wait_class == NO_WAIT_CLASS)
    return HA_ERR_RECORD_DELETED;

  make_row(true, pfs_thread, wait);
  return 0;
}

int table_events_waits_history::delete_all_rows(void)
{
  reset_events_waits_history();
  return 0;
}

PFS_engine_table* table_events_waits_history_long::create(void)
{
  return new table_events_waits_history_long();
}

table_events_waits_history_long::table_events_waits_history_long()
  : table_events_waits_common(&m_share, &m_pos),
  m_pos(0), m_next_pos(0)
{}

void table_events_waits_history_long::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_events_waits_history_long::rnd_next(void)
{
  PFS_events_waits *wait;
  uint limit;

  if (events_waits_history_long_size == 0)
    return HA_ERR_END_OF_FILE;

  if (events_waits_history_long_full)
    limit= events_waits_history_long_size;
  else
    limit= events_waits_history_long_index % events_waits_history_long_size;

  for (m_pos.set_at(&m_next_pos); m_pos.m_index < limit; m_pos.next())
  {
    wait= &events_waits_history_long_array[m_pos.m_index];

    if (wait->m_wait_class != NO_WAIT_CLASS)
    {
      make_row(false, wait->m_thread, wait);
      /* Next iteration, look for the next entry */
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_waits_history_long::rnd_pos(const void *pos)
{
  PFS_events_waits *wait;
  uint limit;

  if (events_waits_history_long_size == 0)
    return HA_ERR_RECORD_DELETED;

  set_position(pos);

  if (events_waits_history_long_full)
    limit= events_waits_history_long_size;
  else
    limit= events_waits_history_long_index % events_waits_history_long_size;

  if (m_pos.m_index >= limit)
    return HA_ERR_RECORD_DELETED;

  wait= &events_waits_history_long_array[m_pos.m_index];

  if (wait->m_wait_class == NO_WAIT_CLASS)
    return HA_ERR_RECORD_DELETED;

  make_row(false, wait->m_thread, wait);
  return 0;
}

int table_events_waits_history_long::delete_all_rows(void)
{
  reset_events_waits_history_long();
  return 0;
}

