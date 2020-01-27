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
  @file storage/perfschema/table_file_instances.cc
  Table FILE_INSTANCES (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "pfs_instr.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_file_instances.h"
#include "pfs_global.h"

THR_LOCK table_file_instances::m_table_lock;

PFS_engine_table_share
table_file_instances::m_share=
{
  { C_STRING_WITH_LEN("file_instances") },
  &pfs_readonly_acl,
  &table_file_instances::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE file_instances("
                      "FILE_NAME VARCHAR(512) not null,"
                      "EVENT_NAME VARCHAR(128) not null,"
                      "OPEN_COUNT INTEGER unsigned not null)") }
};

PFS_engine_table* table_file_instances::create(void)
{
  return new table_file_instances();
}

table_file_instances::table_file_instances()
  : PFS_engine_table(&m_share, &m_pos),
  m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_file_instances::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_file_instances::rnd_next(void)
{
  PFS_file *pfs;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < file_max;
       m_pos.next())
  {
    pfs= &file_array[m_pos.m_index];
    if (pfs->m_lock.is_populated())
    {
      make_row(pfs);
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_file_instances::rnd_pos(const void *pos)
{
  PFS_file *pfs;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < file_max);
  pfs= &file_array[m_pos.m_index];

  if (! pfs->m_lock.is_populated())
    return HA_ERR_RECORD_DELETED;

  make_row(pfs);
  return 0;
}

void table_file_instances::make_row(PFS_file *pfs)
{
  pfs_lock lock;
  PFS_file_class *safe_class;

  m_row_exists= false;

  /* Protect this reader against a file delete */
  pfs->m_lock.begin_optimistic_lock(&lock);

  safe_class= sanitize_file_class(pfs->m_class);
  if (unlikely(safe_class == NULL))
    return;

  m_row.m_filename= pfs->m_filename;
  m_row.m_filename_length= pfs->m_filename_length;
  m_row.m_event_name= safe_class->m_name;
  m_row.m_event_name_length= safe_class->m_name_length;
  m_row.m_open_count= pfs->m_file_stat.m_open_count;

  if (pfs->m_lock.end_optimistic_lock(&lock))
    m_row_exists= true;
}

int table_file_instances::read_row_values(TABLE *table,
                                          unsigned char *,
                                          Field **fields,
                                          bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* FILENAME */
        set_field_varchar_utf8(f, m_row.m_filename, m_row.m_filename_length);
        break;
      case 1: /* EVENT_NAME */
        set_field_varchar_utf8(f, m_row.m_event_name,
                               m_row.m_event_name_length);
        break;
      case 2: /* OPEN_COUNT */
        set_field_ulong(f, m_row.m_open_count);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

