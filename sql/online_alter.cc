/*
   Copyright (c) 2023, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "my_global.h"
#include "handler.h"
#include "sql_class.h"
#include "log_cache.h"


static handlerton *online_alter_hton;


class online_alter_cache_data: public Sql_alloc, public ilist_node<>,
                               public binlog_cache_data
{
public:
  void store_prev_position()
  {
    before_stmt_pos= my_b_write_tell(&cache_log);
  }

  handlerton *hton;
  Cache_flip_event_log *sink_log;
  SAVEPOINT *sv_list;
};


static
online_alter_cache_data *setup_cache_data(MEM_ROOT *root, TABLE_SHARE *share)
{
  static ulong online_alter_cache_use= 0, online_alter_cache_disk_use= 0;

  auto cache= new (root) online_alter_cache_data();
  if (!cache || open_cached_file(&cache->cache_log, mysql_tmpdir, LOG_PREFIX,
                                 (size_t)binlog_cache_size, MYF(MY_WME)))
  {
    delete cache;
    return NULL;
  }

  share->online_alter_binlog->acquire();
  cache->hton= share->db_type();
  cache->sink_log= share->online_alter_binlog;

  my_off_t binlog_max_size= SIZE_T_MAX; // maximum possible cache size
  DBUG_EXECUTE_IF("online_alter_small_cache", binlog_max_size= 4096;);

  cache->set_binlog_cache_info(binlog_max_size,
                               &online_alter_cache_use,
                               &online_alter_cache_disk_use);
  cache->store_prev_position();
  return cache;
}


static online_alter_cache_data *get_cache_data(THD *thd, TABLE *table)
{
  ilist<online_alter_cache_data> &list= thd->online_alter_cache_list;

  /* we assume it's very rare to have more than one online ALTER running */
  for (auto &cache: list)
  {
    if (cache.sink_log == table->s->online_alter_binlog)
      return &cache;
  }

  MEM_ROOT *root= &thd->transaction->mem_root;
  auto *new_cache_data= setup_cache_data(root, table->s);
  list.push_back(*new_cache_data);

  return new_cache_data;
}


int online_alter_log_row(TABLE* table, const uchar *before_record,
                         const uchar *after_record, Log_func *log_func)
{
  THD *thd= table->in_use;

  if (!table->online_alter_cache)
  {
    table->online_alter_cache= get_cache_data(thd, table);
    trans_register_ha(thd, false, online_alter_hton, 0);
    if (thd->in_multi_stmt_transaction_mode())
      trans_register_ha(thd, true, online_alter_hton, 0);
  }

  // We need to log all columns for the case if alter table changes primary key
  DBUG_ASSERT(!before_record || bitmap_is_set_all(table->read_set));
  MY_BITMAP *old_rpl_write_set= table->rpl_write_set;
  table->rpl_write_set= &table->s->all_set;

  table->online_alter_cache->store_prev_position();
  int error= (*log_func)(thd, table, table->s->online_alter_binlog,
                         table->online_alter_cache,
                         table->file->has_transactions_and_rollback(),
                         BINLOG_ROW_IMAGE_FULL,
                         before_record, after_record);

  table->rpl_write_set= old_rpl_write_set;

  if (unlikely(error))
  {
    table->online_alter_cache->restore_prev_position();
    return HA_ERR_RBR_LOGGING_FAILED;
  }

  return 0;
}


static void
cleanup_cache_list(ilist<online_alter_cache_data> &list, bool ending_trans)
{
  if (ending_trans)
  {
    auto it= list.begin();
    while (it != list.end())
    {
      auto &cache= *it++;
      cache.sink_log->release();
      cache.reset();
      delete &cache;
    }
    list.clear();
    DBUG_ASSERT(list.empty());
  }
}


static
int online_alter_end_trans(handlerton *hton, THD *thd, bool all, bool commit)
{
  DBUG_ENTER("online_alter_end_trans");
  int error= 0;
  if (thd->online_alter_cache_list.empty())
    DBUG_RETURN(0);

  bool is_ending_transaction= ending_trans(thd, all);

  for (auto &cache: thd->online_alter_cache_list)
  {
    auto *binlog= cache.sink_log;
    DBUG_ASSERT(binlog);
    bool non_trans= cache.hton->flags & HTON_NO_ROLLBACK // Aria
                    || !cache.hton->rollback;
    bool do_commit= (commit && is_ending_transaction) || non_trans;

    if (commit || non_trans)
    {
      // Do not set STMT_END for last event to leave table open in altering thd
      error= binlog_flush_pending_rows_event(thd, false, true, binlog, &cache);
    }

    if (do_commit)
    {
      /*
        If the cache wasn't reinited to write, then it remains empty after
        the last write.
      */
      if (my_b_bytes_in_cache(&cache.cache_log) && likely(!error))
      {
        DBUG_ASSERT(cache.cache_log.type != READ_CACHE);
        mysql_mutex_lock(binlog->get_log_lock());
        error= binlog->write_cache_raw(thd, &cache.cache_log);
        mysql_mutex_unlock(binlog->get_log_lock());
      }
    }
    else if (!commit) // rollback
    {
      DBUG_ASSERT(!non_trans);
      cache.restore_prev_position();
    }
    else
    {
      DBUG_ASSERT(!is_ending_transaction);
      cache.store_prev_position();
    }


    if (error)
    {
      my_error(ER_ERROR_ON_WRITE, MYF(ME_ERROR_LOG),
               binlog->get_name(), errno);
      cleanup_cache_list(thd->online_alter_cache_list,
                         is_ending_transaction);
      DBUG_RETURN(error);
    }
  }

  cleanup_cache_list(thd->online_alter_cache_list,
                     is_ending_transaction);

  for (TABLE *table= thd->open_tables; table; table= table->next)
    table->online_alter_cache= NULL;
  DBUG_RETURN(error);
}

SAVEPOINT** find_savepoint_in_list(THD *thd, LEX_CSTRING name,
                                   SAVEPOINT ** const list);

SAVEPOINT* savepoint_add(THD *thd, LEX_CSTRING name, SAVEPOINT **list,
                         int (*release_old)(THD*, SAVEPOINT*));

int online_alter_savepoint_set(THD *thd, LEX_CSTRING name)
{
  DBUG_ENTER("binlog_online_alter_savepoint");
  if (thd->online_alter_cache_list.empty())
    DBUG_RETURN(0);

  if (savepoint_alloc_size < sizeof (SAVEPOINT) + sizeof(my_off_t))
    savepoint_alloc_size= sizeof (SAVEPOINT) + sizeof(my_off_t);

  for (auto &cache: thd->online_alter_cache_list)
  {
    if (cache.hton->savepoint_set == NULL)
      continue;

    SAVEPOINT *sv= savepoint_add(thd, name, &cache.sv_list, NULL);
    if(unlikely(sv == NULL))
      DBUG_RETURN(1);
    my_off_t *pos= (my_off_t*)(sv+1);
    *pos= cache.get_byte_position();

    sv->prev= cache.sv_list;
    cache.sv_list= sv;
  }
  DBUG_RETURN(0);
}

int online_alter_savepoint_rollback(THD *thd, LEX_CSTRING name)
{
  DBUG_ENTER("online_alter_savepoint_rollback");
  for (auto &cache: thd->online_alter_cache_list)
  {
    if (cache.hton->savepoint_set == NULL)
      continue;

    SAVEPOINT **sv= find_savepoint_in_list(thd, name, &cache.sv_list);
    // sv is null if savepoint was set up before online table was modified
    my_off_t pos= *sv ? *(my_off_t*)(*sv+1) : 0;

    cache.restore_savepoint(pos);
  }

  DBUG_RETURN(0);
}


static int online_alter_close_connection(handlerton *hton, THD *thd)
{
  DBUG_ASSERT(thd->online_alter_cache_list.empty());
  return 0;
}


static int online_alter_log_init(void *p)
{
  online_alter_hton= (handlerton *)p;
  online_alter_hton->db_type= DB_TYPE_ONLINE_ALTER;
  online_alter_hton->savepoint_offset= sizeof(my_off_t);
  online_alter_hton->close_connection= online_alter_close_connection;

  online_alter_hton->savepoint_set= // Done by online_alter_savepoint_set
          [](handlerton *, THD *, void *){ return 0; };
  online_alter_hton->savepoint_rollback= // Done by online_alter_savepoint_rollback
          [](handlerton *, THD *, void *){ return 0; };
  online_alter_hton->savepoint_rollback_can_release_mdl=
          [](handlerton *hton, THD *thd){ return true; };

  online_alter_hton->commit= [](handlerton *hton, THD *thd, bool all)
  { return online_alter_end_trans(hton, thd, all, true); };
  online_alter_hton->rollback= [](handlerton *hton, THD *thd, bool all)
  { return online_alter_end_trans(hton, thd, all, false); };
  online_alter_hton->commit_by_xid= [](handlerton *hton, XID *xid)
  { return online_alter_end_trans(hton, current_thd, true, true); };
  online_alter_hton->rollback_by_xid= [](handlerton *hton, XID *xid)
  { return online_alter_end_trans(hton, current_thd, true, false); };

  online_alter_hton->drop_table= [](handlerton *, const char*) { return -1; };
  online_alter_hton->flags= HTON_NOT_USER_SELECTABLE | HTON_HIDDEN
                            | HTON_NO_ROLLBACK;
  return 0;
}

struct st_mysql_storage_engine online_alter_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(online_alter_log)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &online_alter_storage_engine,
  "online_alter_log",
  "MariaDB PLC",
  "A pseudo storage engine for the online alter log",
  PLUGIN_LICENSE_GPL,
  online_alter_log_init,
  NULL,
  0x0100, // 1.0
  NULL,   // no status vars
  NULL,   // no sysvars
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
