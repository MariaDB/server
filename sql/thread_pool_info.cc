/* Copyright(C) 2019 MariaDB

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#include <mysql_version.h>

#include <my_global.h>
#include <mysql/plugin.h>
#include <sql_class.h>
#include <sql_i_s.h>
#include <mysql/plugin.h>
#include <sql_show.h>
#include <threadpool_generic.h>

namespace Show {

static ST_FIELD_INFO groups_fields_info[] =
{
  Column("GROUP_ID",        SLong(6), NOT_NULL),
  Column("CONNECTIONS",     SLong(6), NOT_NULL),
  Column("THREADS",         SLong(6), NOT_NULL),
  Column("ACTIVE_THREADS",  SLong(6), NOT_NULL),
  Column("STANDBY_THREADS", SLong(6), NOT_NULL),
  Column("QUEUE_LENGTH",    SLong(6), NOT_NULL),
  Column("HAS_LISTENER",    STiny(1), NOT_NULL),
  Column("IS_STALLED",      STiny(1), NOT_NULL),
  CEnd()
};

} // namespace Show


static int groups_fill_table(THD* thd, TABLE_LIST* tables, COND*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint i = 0; i < threadpool_max_size && all_groups[i].pollfd != INVALID_HANDLE_VALUE; i++)
  {
    thread_group_t* group = &all_groups[i];
    /* ID */
    table->field[0]->store(i, true);
    /* CONNECTION_COUNT */
    table->field[1]->store(group->connection_count, true);
    /* THREAD_COUNT */
    table->field[2]->store(group->thread_count, true);
    /* ACTIVE_THREAD_COUNT */
    table->field[3]->store(group->active_thread_count, true);
    /* STANDBY_THREAD_COUNT */
    table->field[4]->store(group->waiting_threads.elements(), true);
    /* QUEUE LENGTH */
    uint queue_len = group->queues[TP_PRIORITY_LOW].elements()
      + group->queues[TP_PRIORITY_HIGH].elements();
    table->field[5]->store(queue_len, true);
    /* HAS_LISTENER */
    table->field[6]->store((longlong)(group->listener != 0), true);
    /* IS_STALLED */
    table->field[7]->store(group->stalled, true);

    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}


static int groups_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::groups_fields_info;
  schema->fill_table = groups_fill_table;
  return 0;
}


namespace Show {

static ST_FIELD_INFO queues_field_info[] =
{
  Column("GROUP_ID",                   SLong(6),      NOT_NULL),
  Column("POSITION",                   SLong(6),      NOT_NULL),
  Column("PRIORITY",                   SLong(1),      NOT_NULL),
  Column("CONNECTION_ID",              ULonglong(19), NULLABLE),
  Column("QUEUEING_TIME_MICROSECONDS", SLonglong(19), NOT_NULL),
  CEnd()
};

} // namespace Show

typedef connection_queue_t::Iterator connection_queue_iterator;

static int queues_fill_table(THD* thd, TABLE_LIST* tables, COND*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint group_id = 0;
    group_id < threadpool_max_size && all_groups[group_id].pollfd != INVALID_HANDLE_VALUE;
    group_id++)
  {
    thread_group_t* group = &all_groups[group_id];

    mysql_mutex_lock(&group->mutex);
    bool err = false;
    int pos = 0;
    ulonglong now = microsecond_interval_timer();
    for (uint prio = 0; prio < NQUEUES && !err; prio++)
    {
      connection_queue_iterator it(group->queues[prio]);
      TP_connection_generic* c;
      while ((c = it++) != 0)
      {
        /* GROUP_ID */
        table->field[0]->store(group_id, true);
        /* POSITION */
        table->field[1]->store(pos++, true);
        /* PRIORITY */
        table->field[2]->store(prio, true);
        /* CONNECTION_ID */
        if (c->thd)
        {
          table->field[3]->set_notnull();
          table->field[3]->store(c->thd->thread_id, true);
        }
        /* QUEUEING_TIME */
        table->field[4]->store(now - c->enqueue_time, true);

        err = schema_table_store_record(thd, table);
        if (err)
          break;
      }
    }
    mysql_mutex_unlock(&group->mutex);
    if (err)
      return 1;
  }
  return 0;
}

static int queues_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::queues_field_info;
  schema->fill_table = queues_fill_table;
  return 0;
}

namespace Show {

static ST_FIELD_INFO stats_fields_info[] =
{
  Column("GROUP_ID",                      SLong(6),      NOT_NULL),
  Column("THREAD_CREATIONS",              SLonglong(19), NOT_NULL),
  Column("THREAD_CREATIONS_DUE_TO_STALL", SLonglong(19), NOT_NULL),
  Column("WAKES",                         SLonglong(19), NOT_NULL),
  Column("WAKES_DUE_TO_STALL",            SLonglong(19), NOT_NULL),
  Column("THROTTLES",                     SLonglong(19), NOT_NULL),
  Column("STALLS",                        SLonglong(19), NOT_NULL),
  Column("POLLS_BY_LISTENER",             SLonglong(19), NOT_NULL),
  Column("POLLS_BY_WORKER",               SLonglong(19), NOT_NULL),
  Column("DEQUEUES_BY_LISTENER",          SLonglong(19), NOT_NULL),
  Column("DEQUEUES_BY_WORKER",            SLonglong(19), NOT_NULL),
  CEnd()
};

} // namespace Show


static int stats_fill_table(THD* thd, TABLE_LIST* tables, COND*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint i = 0; i < threadpool_max_size && all_groups[i].pollfd != INVALID_HANDLE_VALUE; i++)
  {
    table->field[0]->store(i, true);
    thread_group_t* group = &all_groups[i];

    mysql_mutex_lock(&group->mutex);
    thread_group_counters_t* counters = &group->counters;
    table->field[1]->store(counters->thread_creations, true);
    table->field[2]->store(counters->thread_creations_due_to_stall, true);
    table->field[3]->store(counters->wakes, true);
    table->field[4]->store(counters->wakes_due_to_stall, true);
    table->field[5]->store(counters->throttles, true);
    table->field[6]->store(counters->stalls, true);
    table->field[7]->store(counters->polls[(int)operation_origin::LISTENER], true);
    table->field[8]->store(counters->polls[(int)operation_origin::WORKER], true);
    table->field[9]->store(counters->dequeues[(int)operation_origin::LISTENER], true);
    table->field[10]->store(counters->dequeues[(int)operation_origin::WORKER], true);
    mysql_mutex_unlock(&group->mutex);
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

static int stats_reset_table()
{
  if (!all_groups)
    return 0;

  for (uint i = 0; i < threadpool_max_size && all_groups[i].pollfd != INVALID_HANDLE_VALUE; i++)
  {
    thread_group_t* group = &all_groups[i];
    mysql_mutex_lock(&group->mutex);
    memset(&group->counters, 0, sizeof(group->counters));
    mysql_mutex_unlock(&group->mutex);
  }
  return 0;
}

static int stats_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::stats_fields_info;
  schema->fill_table = stats_fill_table;
  schema->reset_table = stats_reset_table;
  return 0;
}


namespace Show {

static ST_FIELD_INFO waits_fields_info[] =
{
  Column("REASON", Varchar(16),   NOT_NULL),
  Column("COUNT",  SLonglong(19), NOT_NULL),
  CEnd()
};

} // namespace Show

/* See thd_wait_type enum for explanation*/
static const LEX_CSTRING wait_reasons[THD_WAIT_LAST] =
{
  {STRING_WITH_LEN("UNKNOWN")},
  {STRING_WITH_LEN("SLEEP")},
  {STRING_WITH_LEN("DISKIO")},
  {STRING_WITH_LEN("ROW_LOCK")},
  {STRING_WITH_LEN("GLOBAL_LOCK")},
  {STRING_WITH_LEN("META_DATA_LOCK")},
  {STRING_WITH_LEN("TABLE_LOCK")},
  {STRING_WITH_LEN("USER_LOCK")},
  {STRING_WITH_LEN("BINLOG")},
  {STRING_WITH_LEN("GROUP_COMMIT")},
  {STRING_WITH_LEN("SYNC")},
  {STRING_WITH_LEN("NET")}
};

extern Atomic_counter<unsigned long long> tp_waits[THD_WAIT_LAST];

static int waits_fill_table(THD* thd, TABLE_LIST* tables, COND*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (auto i = 0; i < THD_WAIT_LAST; i++)
  {
    table->field[0]->store(wait_reasons[i].str, wait_reasons[i].length, system_charset_info);
    table->field[1]->store(tp_waits[i], true);
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

static int waits_reset_table()
{
  for (auto i = 0; i < THD_WAIT_LAST; i++)
    tp_waits[i] = 0;

  return 0;
}

static int waits_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::waits_fields_info;
  schema->fill_table = waits_fill_table;
  schema->reset_table = waits_reset_table;
  return 0;
}

static struct st_mysql_information_schema plugin_descriptor =
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

maria_declare_plugin(thread_pool_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_GROUPS",
  "Vladislav Vaintroub",
  "Provides information about threadpool groups.",
  PLUGIN_LICENSE_GPL,
  groups_init,
  0,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_QUEUES",
  "Vladislav Vaintroub",
  "Provides information about threadpool queues.",
  PLUGIN_LICENSE_GPL,
  queues_init,
  0,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_STATS",
  "Vladislav Vaintroub",
  "Provides performance counter information for threadpool.",
  PLUGIN_LICENSE_GPL,
  stats_init,
  0,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_WAITS",
  "Vladislav Vaintroub",
  "Provides wait counters for threadpool.",
  PLUGIN_LICENSE_GPL,
  waits_init,
  0,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
