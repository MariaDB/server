/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file storage/perfschema/table_setup_consumers.cc
  Table SETUP_CONSUMERS (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "table_setup_consumers.h"
#include "pfs_instr.h"
#include "pfs_events_waits.h"
#include "pfs_digest.h"

#define COUNT_SETUP_CONSUMERS 12
static row_setup_consumers all_setup_consumers_data[COUNT_SETUP_CONSUMERS]=
{
  {
    { C_STRING_WITH_LEN("events_stages_current") },
    &flag_events_stages_current,
    false
  },
  {
    { C_STRING_WITH_LEN("events_stages_history") },
    &flag_events_stages_history,
    false
  },
  {
    { C_STRING_WITH_LEN("events_stages_history_long") },
    &flag_events_stages_history_long,
    false
  },
  {
    { C_STRING_WITH_LEN("events_statements_current") },
    &flag_events_statements_current,
    false
  },
  {
    { C_STRING_WITH_LEN("events_statements_history") },
    &flag_events_statements_history,
    false
  },
  {
    { C_STRING_WITH_LEN("events_statements_history_long") },
    &flag_events_statements_history_long,
    false
  },
  {
    { C_STRING_WITH_LEN("events_waits_current") },
    &flag_events_waits_current,
    false
  },
  {
    { C_STRING_WITH_LEN("events_waits_history") },
    &flag_events_waits_history,
    false
  },
  {
    { C_STRING_WITH_LEN("events_waits_history_long") },
    &flag_events_waits_history_long,
    false
  },
  {
    { C_STRING_WITH_LEN("global_instrumentation") },
    &flag_global_instrumentation,
    true
  },
  {
    { C_STRING_WITH_LEN("thread_instrumentation") },
    &flag_thread_instrumentation,
    false
  },
  {
    { C_STRING_WITH_LEN("statements_digest") },
    &flag_statements_digest,
    false
  }
};

THR_LOCK table_setup_consumers::m_table_lock;

PFS_engine_table_share
table_setup_consumers::m_share=
{
  { C_STRING_WITH_LEN("setup_consumers") },
  &pfs_updatable_acl,
  &table_setup_consumers::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  COUNT_SETUP_CONSUMERS, /* records */
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE setup_consumers("
                      "NAME VARCHAR(64) not null,"
                      "ENABLED ENUM ('YES', 'NO') not null)") }
};

PFS_engine_table* table_setup_consumers::create(void)
{
  return new table_setup_consumers();
}

table_setup_consumers::table_setup_consumers()
  : PFS_engine_table(&m_share, &m_pos),
    m_row(NULL), m_pos(0), m_next_pos(0)
{}

void table_setup_consumers::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_setup_consumers::rnd_next(void)
{
  int result;

  m_pos.set_at(&m_next_pos);

  if (m_pos.m_index < COUNT_SETUP_CONSUMERS)
  {
    m_row= &all_setup_consumers_data[m_pos.m_index];
    m_next_pos.set_after(&m_pos);
    result= 0;
  }
  else
  {
    m_row= NULL;
    result= HA_ERR_END_OF_FILE;
  }

  return result;
}

int table_setup_consumers::rnd_pos(const void *pos)
{
  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < COUNT_SETUP_CONSUMERS);
  m_row= &all_setup_consumers_data[m_pos.m_index];
  return 0;
}

int table_setup_consumers::read_row_values(TABLE *table,
                                           unsigned char *,
                                           Field **fields,
                                           bool read_all)
{
  Field *f;

  DBUG_ASSERT(m_row);


  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* NAME */
        set_field_varchar_utf8(f, m_row->m_name.str, m_row->m_name.length);
        break;
      case 1: /* ENABLED */
        set_field_enum(f, (*m_row->m_enabled_ptr) ? ENUM_YES : ENUM_NO);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

int table_setup_consumers::update_row_values(TABLE *table,
                                             const unsigned char *,
                                             const unsigned char *,
                                             Field **fields)
{
  Field *f;
  enum_yes_no value;

  DBUG_ASSERT(m_row);

  for (; (f= *fields) ; fields++)
  {
    if (bitmap_is_set(table->write_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* NAME */
        return HA_ERR_WRONG_COMMAND;
      case 1: /* ENABLED */
      {
        value= (enum_yes_no) get_field_enum(f);
        *m_row->m_enabled_ptr= (value == ENUM_YES) ? true : false;
        break;
      }
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  if (m_row->m_refresh)
    update_instruments_derived_flags();

  return 0;
}


