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
  @file storage/perfschema/table_replication_applier_configuration.cc
  Table replication_applier_configuration (implementation).
*/

//#define HAVE_REPLICATION

#include "my_global.h"
#ifdef HAVE_REPLICATION
#include "table_replication_applier_configuration.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "slave.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"

THR_LOCK table_replication_applier_configuration::m_table_lock;

PFS_engine_table_share
table_replication_applier_configuration::m_share=
{
  { C_STRING_WITH_LEN("replication_applier_configuration") },
  &pfs_readonly_acl,
  table_replication_applier_configuration::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_applier_configuration::get_row_count,
  sizeof(pos_t), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE replication_applier_configuration("
  "CHANNEL_NAME VARCHAR(256) collate utf8_general_ci not null comment 'Replication channel name.',"
  "DESIRED_DELAY INTEGER not null comment 'Target number of seconds the replica should be delayed to the master.')") },
  false  /* perpetual */
};

PFS_engine_table* table_replication_applier_configuration::create(void)
{
  return new table_replication_applier_configuration();
}

table_replication_applier_configuration
  ::table_replication_applier_configuration()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_applier_configuration
  ::~table_replication_applier_configuration()
{}

void table_replication_applier_configuration::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}


ha_rows table_replication_applier_configuration::get_row_count()
{
 return master_info_index->master_info_hash.records;
}


int table_replication_applier_configuration::rnd_next(void)
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

int table_replication_applier_configuration::rnd_pos(const void *pos)
{
  Master_info *mi;
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

void table_replication_applier_configuration::make_row(Master_info *mi)
{
  m_row_exists= false;

  DBUG_ASSERT(mi != NULL);

  mysql_mutex_lock(&mi->data_lock);
  mysql_mutex_lock(&mi->rli.data_lock);

  m_row.channel_name_length= static_cast<uint>(mi->connection_name.length);
  memcpy(m_row.channel_name, mi->connection_name.str, m_row.channel_name_length);
  m_row.desired_delay= mi->rli.get_sql_delay();

  mysql_mutex_unlock(&mi->rli.data_lock);
  mysql_mutex_unlock(&mi->data_lock);

  m_row_exists= true;
}

int table_replication_applier_configuration::read_row_values(TABLE *table,
                                                             unsigned char *buf,
                                                             Field **fields,
                                                             bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* channel_name */
        set_field_varchar_utf8(f, m_row.channel_name, m_row.channel_name_length);
        break;
      case 1: /* desired_delay */
        set_field_ulong(f, static_cast<ulong>(m_row.desired_delay));
        break;
      default:
        assert(false);
      }
    }
  }
  return 0;
}
#endif
