/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

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
  @file storage/perfschema/table_setup_timers.cc
  Table SETUP_TIMERS (implementation).
*/

#include "my_global.h"
#include "my_pthread.h"
#include "table_setup_timers.h"
#include "pfs_column_values.h"
#include "pfs_timer.h"

#define COUNT_SETUP_TIMERS 4

static row_setup_timers all_setup_timers_data[COUNT_SETUP_TIMERS]=
{
  {
    { C_STRING_WITH_LEN("idle") },
    &idle_timer
  },
  {
    { C_STRING_WITH_LEN("wait") },
    &wait_timer
  },
  {
    { C_STRING_WITH_LEN("stage") },
    &stage_timer
  },
  {
    { C_STRING_WITH_LEN("statement") },
    &statement_timer
  }
};

THR_LOCK table_setup_timers::m_table_lock;

PFS_engine_table_share
table_setup_timers::m_share=
{
  { C_STRING_WITH_LEN("setup_timers") },
  &pfs_updatable_acl,
  &table_setup_timers::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  COUNT_SETUP_TIMERS,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE setup_timers("
                      "NAME VARCHAR(64) not null,"
                      "TIMER_NAME ENUM ('CYCLE', 'NANOSECOND', 'MICROSECOND', 'MILLISECOND', 'TICK') not null)") }
};

PFS_engine_table* table_setup_timers::create(void)
{
  return new table_setup_timers();
}

table_setup_timers::table_setup_timers()
  : PFS_engine_table(&m_share, &m_pos),
    m_row(NULL), m_pos(0), m_next_pos(0)
{}

void table_setup_timers::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_setup_timers::rnd_next(void)
{
  int result;

  m_pos.set_at(&m_next_pos);

  if (m_pos.m_index < COUNT_SETUP_TIMERS)
  {
    m_row= &all_setup_timers_data[m_pos.m_index];
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

int table_setup_timers::rnd_pos(const void *pos)
{
  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < COUNT_SETUP_TIMERS);
  m_row= &all_setup_timers_data[m_pos.m_index];
  return 0;
}

int table_setup_timers::read_row_values(TABLE *table,
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
        set_field_varchar_utf8(f, m_row->m_name.str,(uint) m_row->m_name.length);
        break;
      case 1: /* TIMER_NAME */
        set_field_enum(f, *(m_row->m_timer_name_ptr));
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

int table_setup_timers::update_row_values(TABLE *table,
                                          const unsigned char *,
                                          const unsigned char *,
                                          Field **fields)
{
  Field *f;
  longlong value;

  DBUG_ASSERT(m_row);

  for (; (f= *fields) ; fields++)
  {
    if (bitmap_is_set(table->write_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* NAME */
        return HA_ERR_WRONG_COMMAND;
      case 1: /* TIMER_NAME */
        value= get_field_enum(f);
        if ((value >= FIRST_TIMER_NAME) && (value <= LAST_TIMER_NAME))
          *(m_row->m_timer_name_ptr)= (enum_timer_name) value;
        else
          return HA_ERR_WRONG_COMMAND;
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }

  return 0;
}

