/* Copyright (c) 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/**
  @file storage/perfschema/table_galera_group_members.cc
  Table galera_group_members (implementation).
*/

#include "my_global.h"
#include "table_galera_group_members.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "log.h"

#include <atomic>

THR_LOCK table_galera_group_members::m_table_lock;

PFS_engine_table_share_state
table_galera_group_members::m_share_state = {
  false /* m_checked */
};

#define STRINGIFY2(X) #X
#define STRINGIFY(X) STRINGIFY2(X)

PFS_engine_table_share
table_galera_group_members::m_share=
{
  { C_STRING_WITH_LEN("galera_group_members") },
  &pfs_readonly_acl,
  &table_galera_group_members::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_galera_group_members::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE galera_group_members("
                      "wsrep_node_id char(" STRINGIFY(WSREP_UUID_STR_LEN) ") not null comment 'Unique node ID (UUID)',"
                      "wsrep_local_index INTEGER UNSIGNED not null comment 'Index of this node in the Galera cluster nodes table',"
                      "wsrep_cluster_state_uuid char(" STRINGIFY(WSREP_UUID_STR_LEN) ") not null comment 'The UUID of the cluster',"
                      "wsrep_local_state_uuid char(" STRINGIFY(WSREP_UUID_STR_LEN) ") not null comment 'The UUID of the state stored on this node',"
                      "wsrep_last_applied BIGINT UNSIGNED not null comment 'Sequence number of the last applied transaction',"
                      "wsrep_last_committed BIGINT UNSIGNED not null comment 'Sequence number of the last committed transaction',"
                      "wsrep_replicated BIGINT UNSIGNED not null comment 'Total number of write-sets replicated',"
                      "wsrep_replicated_bytes BIGINT UNSIGNED not null comment 'Total size of write-sets replicated (in bytes)',"
                      "wsrep_received BIGINT UNSIGNED not null comment 'Total number of write-sets received',"
                      "wsrep_received_bytes BIGINT UNSIGNED not null comment 'Total size of write-sets received (in bytes)',"
                      "wsrep_local_bf_aborts BIGINT UNSIGNED not null comment 'Total number of local transactions that were aborted by slave transactions while in execution',"
                      "wsrep_local_commits BIGINT UNSIGNED not null comment 'Total number of local transactions committed',"
                      "wsrep_local_cert_failures BIGINT UNSIGNED not null comment 'Total number of local transactions that failed certification test',"
                      "wsrep_apply_window DOUBLE PRECISION not null comment 'Average distance between the highest and lowest concurrently applied seqno',"
                      "wsrep_commit_window DOUBLE PRECISION not null comment 'Average distance between the highest and lowest concurrently committed seqno')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table* table_galera_group_members::create()
{
  return new table_galera_group_members();
}

static std::atomic<wsrep_ps_fetch_cluster_info_t> fetch_cluster_info;
static std::atomic<wsrep_ps_free_cluster_info_t> free_cluster_info;

static wsrep_ps_fetch_cluster_info_t init_once()
{
  if (auto f= fetch_cluster_info.load(std::memory_order_acquire))
    return f;

  auto dlh= static_cast<wsrep_t*>
    (Wsrep_server_state::instance().get_provider().native())->dlh;

  union
  {
    void* sym;
    wsrep_ps_fetch_cluster_info_t func;
  } obj;

  obj.sym= dlsym(dlh, WSREP_PS_FETCH_CLUSTER_INFO_FUNC);

  union
  {
    void* sym;
    wsrep_ps_free_cluster_info_t func;
  } obj2;

  obj2.sym= obj.func ? dlsym(dlh, WSREP_PS_FREE_CLUSTER_INFO_FUNC) : nullptr;

  if (!obj.sym || !obj2.sym)
  {
    WSREP_WARN("Performance Schema for Galera: incompatible or "
               "old version of the Galera library");
    return nullptr;
  }

  DBUG_PRINT("info", ("Initialized galera PS fetch cluster info: %p %p",
                      dlh, obj.sym));

  free_cluster_info.store(obj2.func, std::memory_order_relaxed);
  fetch_cluster_info.store(obj.func, std::memory_order_release);

  return obj.func;
}

table_galera_group_members::table_galera_group_members()
  : PFS_engine_table(&m_share, &m_pos)
{
}

static void free_rows(wsrep_node_info_t* entries)
{
  if (entries)
  {
    auto free_func= free_cluster_info.load(std::memory_order_relaxed);
    free_func(static_cast<wsrep_t*>
              (Wsrep_server_state::instance().get_provider().native()),
              entries);
  }
}

table_galera_group_members::~table_galera_group_members()
{
  free_rows(m_entries);
}

void table_galera_group_members::reset_position()
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_galera_group_members::get_row_count()
{
  return WSREP_ON && init_once() ? wsrep_cluster_size : 0;
}

int table_galera_group_members::rnd_init(bool)
{
  if (!WSREP_ON)
    return 0;
  if (auto fetch= init_once())
  {
    free_rows(m_entries);
    m_entries= nullptr;
    m_rows= 0;
    uint32_t size;
    int32_t my_index;
    wsrep_node_info_t* entries;
    wsrep_status_t ret=
      fetch(static_cast<wsrep_t*>
            (Wsrep_server_state::instance().get_provider().native()),
            &entries, &size, &my_index, WSREP_PS_API_VERSION);
    if (ret == WSREP_OK && size &&
        entries[0].wsrep_version <= WSREP_PS_API_VERSION)
    {
      m_entries= entries;
      m_rows= size;
    }
  }
  return 0;
}

int table_galera_group_members::rnd_next()
{
  if (!WSREP_ON)
    return HA_ERR_END_OF_FILE;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < m_rows;
       m_pos.next())
  {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_galera_group_members::rnd_pos(const void *pos)
{
  if (!WSREP_ON)
    return HA_ERR_END_OF_FILE;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < m_rows);
  make_row(m_pos.m_index);

  return 0;
}

void table_galera_group_members::make_row(uint index)
{
  DBUG_ENTER("table_galera_group_members::make_row");

  // Set default values.
  m_row_exists= false;

  if (index >= m_rows) {
    DBUG_ASSERT(false);
  }

  // Query plugin and let callbacks do their job.
  if (WSREP_ON)
  {
    m_row_exists= true;
    m_row= m_entries[index];
  }
  else
  {
    DBUG_PRINT("info", ("Galera stats not available!"));
  }

  DBUG_VOID_RETURN;
}

int table_galera_group_members::read_row_values(TABLE *table,
                                                     unsigned char *buf,
                                                     Field **fields,
                                                     bool read_all)
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
      case 0: /** wsrep_node_id */
        set_field_char_utf8(f, m_row.wsrep_node_id,
                            WSREP_UUID_STR_LEN);
        break;
      case 1: /** wsrep_local_index */
        set_field_ulong(f, m_row.wsrep_local_index);
        break;
      case 2: /** wsrep_cluster_state_uuid */
        set_field_char_utf8(f, m_row.wsrep_cluster_state_uuid,
                            WSREP_UUID_STR_LEN);
        break;
      case 3: /** wsrep_local_state_uuid */
        set_field_char_utf8(f, m_row.wsrep_local_state_uuid,
                            WSREP_UUID_STR_LEN);
        break;
      case 4: /** wsrep_last_applied */
        set_field_ulonglong(f, m_row.wsrep_last_applied);
        break;
      case 5: /** wsrep_last_committed */
        set_field_ulonglong(f, m_row.wsrep_last_committed);
        break;
      case 6: /** wsrep_replicated */
        set_field_ulonglong(f, m_row.wsrep_replicated);
        break;
      case 7: /** wsrep_replicated_bytes */
        set_field_ulonglong(f, m_row.wsrep_replicated_bytes);
        break;
      case 8: /** wsrep_received */
        set_field_ulonglong(f, m_row.wsrep_received);
        break;
      case 9: /** wsrep_received_bytes */
        set_field_ulonglong(f, m_row.wsrep_received_bytes);
        break;
      case 10: /** wsrep_local_bf_aborts */
        set_field_ulonglong(f, m_row.wsrep_local_bf_aborts);
        break;
      case 11: /** wsrep_local_commits */
        set_field_ulonglong(f, m_row.wsrep_local_commits);
        break;
      case 12: /** wsrep_local_cert_failures */
        set_field_ulonglong(f, m_row.wsrep_local_cert_failures);
        break;
      case 13: /** wsrep_apply_window */
        set_field_double(f, m_row.wsrep_apply_window);
        break;
      case 14: /** wsrep_commit_window */
        set_field_double(f, m_row.wsrep_commit_window);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}
