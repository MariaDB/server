/* Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.

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
  @file storage/perfschema/table_socket_instances.cc
  Table SOCKET_INSTANCES (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "pfs_instr.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_socket_instances.h"
#include "pfs_global.h"

THR_LOCK table_socket_instances::m_table_lock;

PFS_engine_table_share
table_socket_instances::m_share=
{
  { C_STRING_WITH_LEN("socket_instances") },
  &pfs_readonly_acl,
  &table_socket_instances::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE socket_instances("
                      "EVENT_NAME VARCHAR(128) not null,"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
                      "THREAD_ID BIGINT unsigned,"
                      "SOCKET_ID INTEGER not null,"
                      "IP VARCHAR(64) not null,"
                      "PORT INTEGER not null,"
                      "STATE ENUM('IDLE','ACTIVE') not null)") }
};

PFS_engine_table* table_socket_instances::create(void)
{
  return new table_socket_instances();
}

table_socket_instances::table_socket_instances()
  : PFS_engine_table(&m_share, &m_pos),
  m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_socket_instances::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_socket_instances::rnd_next(void)
{
  PFS_socket *pfs;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < socket_max;
       m_pos.next())
  {
    pfs= &socket_array[m_pos.m_index];
    if (pfs->m_lock.is_populated())
    {
      make_row(pfs);
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_socket_instances::rnd_pos(const void *pos)
{
  PFS_socket *pfs;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < socket_max);
  pfs= &socket_array[m_pos.m_index];

  if (! pfs->m_lock.is_populated())
    return HA_ERR_RECORD_DELETED;

  make_row(pfs);
  return 0;
}

void table_socket_instances::make_row(PFS_socket *pfs)
{
  pfs_lock lock;
  PFS_socket_class *safe_class;

  m_row_exists= false;

  /* Protect this reader against a socket delete */
  pfs->m_lock.begin_optimistic_lock(&lock);

  safe_class= sanitize_socket_class(pfs->m_class);
  if (unlikely(safe_class == NULL))
    return;

  /** Extract ip address and port from raw address */
  m_row.m_ip_length= pfs_get_socket_address(m_row.m_ip, sizeof(m_row.m_ip),
                                            &m_row.m_port,
                                            &pfs->m_sock_addr, pfs->m_addr_len);
  m_row.m_event_name=        safe_class->m_name;
  m_row.m_event_name_length= safe_class->m_name_length;
  m_row.m_identity=          pfs->m_identity;
  m_row.m_fd=                pfs->m_fd;
  m_row.m_state=             (pfs->m_idle ? PSI_SOCKET_STATE_IDLE
                                          : PSI_SOCKET_STATE_ACTIVE);
  PFS_thread *safe_thread= sanitize_thread(pfs->m_thread_owner);

  if (safe_thread != NULL)
  {
    m_row.m_thread_id= safe_thread->m_thread_internal_id;
    m_row.m_thread_id_set= true;
  }
  else
    m_row.m_thread_id_set= false;


  if (pfs->m_lock.end_optimistic_lock(&lock))
    m_row_exists= true;
}

int table_socket_instances::read_row_values(TABLE *table,
                                          unsigned char *buf,
                                          Field **fields,
                                          bool read_all)
{
  Field *f;

  if (unlikely(!m_row_exists))
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
      case 0: /* EVENT_NAME */
        set_field_varchar_utf8(f, m_row.m_event_name, m_row.m_event_name_length);
        break;
      case 1: /* OBJECT_INSTANCE_BEGIN */
        set_field_ulonglong(f, (intptr)m_row.m_identity);
        break;
      case 2: /* THREAD_ID */
        if (m_row.m_thread_id_set)
          set_field_ulonglong(f, m_row.m_thread_id);
        else
          f->set_null();
        break;
      case 3: /* SOCKET_ID */
        set_field_ulong(f, m_row.m_fd);
        break;
      case 4: /* IP */
        set_field_varchar_utf8(f, m_row.m_ip, m_row.m_ip_length);
        break;
      case 5: /* PORT */
        set_field_ulong(f, m_row.m_port);
        break;
      case 6: /* STATE */
        set_field_enum(f, m_row.m_state);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

