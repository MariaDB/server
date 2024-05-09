/* Copyright (c) 2021-2024, MariaDB Corporation.

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

#include "my_global.h"

#ifdef WITH_WSREP

/**
  @file storage/perfschema/table_galera_group_member_stats.cc
  Table galera_group_member_stats (implementation).
*/

#include "table_galera_group_member_stats.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "wsrep.h"
#include "wsrep_server_state.h"
#include "wsrep_mysqld.h"

#include <atomic>

THR_LOCK table_galera_group_member_stats::m_table_lock;

PFS_engine_table_share_state
table_galera_group_member_stats::m_share_state = {
  false /* m_checked */
};

#define STRINGIFY2(X) #X
#define STRINGIFY(X) STRINGIFY2(X)

PFS_engine_table_share
table_galera_group_member_stats::m_share=
{
  { C_STRING_WITH_LEN("galera_group_member_stats") },
  &pfs_readonly_acl,
  &table_galera_group_member_stats::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_galera_group_member_stats::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE galera_group_member_stats("
                      "wsrep_node_id char(" STRINGIFY(WSREP_UUID_STR_LEN) ") not null comment 'Unique node ID (UUID)',"
                      "wsrep_local_index INTEGER UNSIGNED not null comment 'Index of this node in the Galera cluster nodes table',"
                      "wsrep_repl_keys BIGINT UNSIGNED not null comment 'Total number of keys replicated',"
                      "wsrep_repl_keys_bytes BIGINT UNSIGNED not null comment 'Total size of keys replicated (in bytes)',"
                      "wsrep_repl_data_bytes BIGINT UNSIGNED not null comment 'Total size of data replicated (in bytes)',"
                      "wsrep_repl_other_bytes BIGINT UNSIGNED not null comment 'Total size of other bits replicated (in bytes)',"
                      "wsrep_local_replays BIGINT UNSIGNED not null comment 'Total number of transaction replays due to asymmetric lock granularity',"
                      "wsrep_local_send_queue BIGINT UNSIGNED not null comment 'Current (instantaneous) length of the send queue',"
                      "wsrep_local_send_queue_avg DOUBLE PRECISION not null comment 'Send queue length averaged over time since the last FLUSH STATUS command',"
                      "wsrep_local_recv_queue BIGINT UNSIGNED not null comment 'Current (instantaneous) length of the receive queue',"
                      "wsrep_local_recv_queue_avg DOUBLE PRECISION not null comment 'Receive queue length averaged over interval since the last FLUSH STATUS command',"
                      "wsrep_flow_control_paused BIGINT UNSIGNED not null comment 'The fraction of time (out of 1.0) since the last SHOW GLOBAL STATUS that flow control is effective',"
                      "wsrep_flow_control_sent BIGINT UNSIGNED not null comment 'The number of flow control messages sent by the local node to the cluster',"
                      "wsrep_flow_control_recv BIGINT UNSIGNED not null comment 'The number of flow control messages the node has received, including those the node has sent',"
                      "wsrep_flow_control_status VARCHAR("
                         STRINGIFY(WSREP_STATUS_LENGTH)
                      ") not null comment 'Status shows whether a node has flow control enabled for normal traffic',"
                      "wsrep_cert_deps_distance DOUBLE PRECISION not null comment 'Average distance between the highest and lowest seqno value that can be possibly applied in parallel',"
                      "wsrep_open_transactions BIGINT UNSIGNED not null comment 'The number of locally running transactions which have been registered inside the wsrep provider',"
                      "wsrep_evs_repl_latency BIGINT UNSIGNED not null comment 'This status variable provides figures for the replication latency on group communication')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table* table_galera_group_member_stats::create()
{
  return new table_galera_group_member_stats();
}

static std::atomic<wsrep_ps_fetch_node_stat_t> fetch_node_stat;
static std::atomic<wsrep_ps_free_node_stat_t> free_node_stat;

static wsrep_ps_fetch_node_stat_t init_once()
{
  if (auto f= fetch_node_stat.load(std::memory_order_acquire))
    return f;

  auto dlh= static_cast<wsrep_t*>
    (Wsrep_server_state::instance().get_provider().native())->dlh;

  union
  {
    void* sym;
    wsrep_ps_fetch_node_stat_t func;
  } obj;

  obj.sym= dlsym(dlh, WSREP_PS_FETCH_NODE_STAT_FUNC);

  union
  {
    void* sym;
    wsrep_ps_free_node_stat_t func;
  } obj2;

  obj2.sym= obj.func ? dlsym(dlh, WSREP_PS_FREE_NODE_STAT_FUNC) : nullptr;

  if (!obj.sym || !obj2.sym)
  {
    WSREP_WARN("Performance Schema for Galera: incompatible or "
               "old version of the Galera library");
    return nullptr;
  }

  DBUG_PRINT("info", ("Initialized galera PS fetch node stat: %p %p",
                      dlh, obj.sym));

  free_node_stat.store(obj2.func, std::memory_order_relaxed);
  fetch_node_stat.store(obj.func, std::memory_order_release);

  return obj.func;
}

table_galera_group_member_stats::table_galera_group_member_stats()
  : PFS_engine_table(&m_share, &m_pos)
{
}

static void free_rows(wsrep_node_stat_t* entries)
{
  if (entries)
  {
    auto free_func= free_node_stat.load(std::memory_order_relaxed);
    free_func(static_cast<wsrep_t*>
              (Wsrep_server_state::instance().get_provider().native()),
              entries);
  }
}

table_galera_group_member_stats::~table_galera_group_member_stats()
{
  free_rows(m_entries);
}

void table_galera_group_member_stats::reset_position()
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_galera_group_member_stats::get_row_count()
{
  return WSREP_ON && init_once() && wsrep_cluster_size > 0;
}

int table_galera_group_member_stats::rnd_init(bool)
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
    wsrep_node_stat_t* entries;
    wsrep_status_t ret=
      fetch(static_cast<wsrep_t*>
            (Wsrep_server_state::instance().get_provider().native()),
            &entries, &size, &my_index, WSREP_PS_API_VERSION);
    if (ret == WSREP_OK && size && my_index >= 0 &&
        entries[0].wsrep_version <= WSREP_PS_API_VERSION)
    {
      m_entries= entries;
      m_rows= size;
    }
  }
  return 0;
}

int table_galera_group_member_stats::rnd_next()
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

int table_galera_group_member_stats::rnd_pos(const void *pos)
{
  if (!WSREP_ON)
    return HA_ERR_END_OF_FILE;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < m_rows);
  make_row(m_pos.m_index);

  return 0;
}

void table_galera_group_member_stats::make_row(uint index)
{
  DBUG_ENTER("table_galera_group_member_stats::make_row");

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

int table_galera_group_member_stats::read_row_values(TABLE *table,
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
      case 0: /** wsrep_node_id */
        set_field_char_utf8(f, m_row.wsrep_node_id,
                            WSREP_UUID_STR_LEN);
        break;
      case 1: /** wsrep_local_index */
        set_field_ulong(f, m_row.wsrep_local_index);
        break;
      case 2: /** wsrep_repl_keys */
        set_field_ulonglong(f, m_row.wsrep_repl_keys);
        break;
      case 3: /** wsrep_repl_keys_bytes */
        set_field_ulonglong(f, m_row.wsrep_repl_keys_bytes);
        break;
      case 4: /** wsrep_repl_data_bytes */
        set_field_ulonglong(f, m_row.wsrep_repl_data_bytes);
        break;
      case 5: /** wsrep_repl_other_bytes */
        set_field_ulonglong(f, m_row.wsrep_repl_other_bytes);
        break;
      case 6: /** wsrep_local_replays */
        set_field_ulonglong(f, m_row.wsrep_local_replays);
        break;
      case 7: /** wsrep_local_send_queue */
        set_field_ulonglong(f, m_row.wsrep_local_send_queue);
        break;
      case 8: /** wsrep_local_send_queue_avg */
        set_field_double(f, m_row.wsrep_local_send_queue_avg);
        break;
      case 9: /** wsrep_local_recv_queue */
        set_field_ulonglong(f, m_row.wsrep_local_recv_queue);
        break;
      case 10: /** wsrep_local_recv_queue_avg */
        set_field_double(f, m_row.wsrep_local_recv_queue_avg);
        break;
      case 11: /** wsrep_flow_control_paused */
        set_field_ulonglong(f, m_row.wsrep_flow_control_paused);
        break;
      case 12: /** wsrep_flow_control_sent */
        set_field_ulonglong(f, m_row.wsrep_flow_control_sent);
        break;
      case 13: /** wsrep_flow_control_recv */
        set_field_ulonglong(f, m_row.wsrep_flow_control_recv);
        break;
      case 14: /** wsrep_flow_control_status */
        set_field_varchar_utf8(f, m_row.wsrep_flow_control_status,
                               strlen(m_row.wsrep_flow_control_status));
        break;
      case 15: /** wsrep_cert_deps_distance */
        set_field_double(f, m_row.wsrep_cert_deps_distance);
        break;
      case 16: /** wsrep_open_transactions */
        set_field_ulonglong(f, m_row.wsrep_open_transactions);
        break;
      case 17: /** wsrep_evs_repl_latency */
        set_field_ulonglong(f, m_row.wsrep_evs_repl_latency);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}

#endif
