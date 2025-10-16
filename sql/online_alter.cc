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

typedef void *sv_id_t;

struct Online_alter_cache_list: ilist<online_alter_cache_data>
{
  sv_id_t savepoint_id= 0;
};

struct table_savepoint: ilist_node<>
{
  sv_id_t id;
  my_off_t log_prev_pos;
  table_savepoint(sv_id_t id, my_off_t pos): id(id), log_prev_pos(pos){}
};

class online_alter_cache_data: public ilist_node<>,
                               public binlog_cache_data
{
public:
  online_alter_cache_data() : binlog_cache_data(false, false),
    hton(nullptr), sink_log(nullptr) { }
  void store_prev_position()
  {
    before_stmt_pos= my_b_write_tell(&cache_log);
  }

  handlerton *hton;
  Cache_flip_event_log *sink_log;
  ilist<table_savepoint> sv_list;
  /**
    Finds savepoint with specified id and returns its associated data.
    Cleans up all the savepoints up to the found one.
  */
  my_off_t pop_sv_until(sv_id_t id)
  {
    my_off_t pos= 0;
    auto it= sv_list.begin();
    auto sentinel= it->prev;
    while (pos == 0 && it != sv_list.end())
    {
      table_savepoint &sv= *it;
      ++it;
      if (sv.id == id)
        pos= sv.log_prev_pos;
      delete &sv;
    }
    sentinel->next= &*it; // drop the range from the list
    it->prev= sentinel;
    return pos;
  }
  void cleanup_sv()
  {
    pop_sv_until((sv_id_t)1); // Erase the whole list
  }
};


static
online_alter_cache_data *setup_cache_data(MEM_ROOT *root, TABLE_SHARE *share)
{
  static ulong online_alter_cache_use= 0, online_alter_cache_disk_use= 0;

  auto cache= new online_alter_cache_data();
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


static Online_alter_cache_list &get_cache_list(handlerton *ht, THD *thd)
{
  void *data= thd_get_ha_data(thd, ht);
  DBUG_ASSERT(data);
  return *(Online_alter_cache_list*)data;
}


static Online_alter_cache_list &get_or_create_cache_list(THD *thd)
{
  void *data= thd_get_ha_data(thd, online_alter_hton);
  if (!data)
  {
    data= new Online_alter_cache_list();
    thd_set_ha_data(thd, online_alter_hton, data);
  }
  return *(Online_alter_cache_list*)data;
}


static online_alter_cache_data* get_cache_data(THD *thd, TABLE *table)
{
  auto &cache_list= get_or_create_cache_list(thd);
  /* we assume it's very rare to have more than one online ALTER running */
  for (auto &cache: cache_list)
  {
    if (cache.sink_log == table->s->online_alter_binlog)
      return &cache;
  }

  MEM_ROOT *root= &thd->transaction->mem_root;
  auto *new_cache_data= setup_cache_data(root, table->s);
  cache_list.push_back(*new_cache_data);

  return new_cache_data;
}


int online_alter_log_row(TABLE* table, const uchar *before_record,
                         const uchar *after_record, Log_func *log_func)
{
  THD *thd= table->in_use;

  if (!table->online_alter_cache)
  {
    table->online_alter_cache= get_cache_data(thd, table);
    DBUG_ASSERT(table->online_alter_cache->cache_log.type == WRITE_CACHE);
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


static void cleanup_cache_list(ilist<online_alter_cache_data> &list)
{
  auto it= list.begin();
  while (it != list.end())
  {
    auto &cache= *it++;
    cache.sink_log->release();
    cache.reset();
    cache.cleanup_sv();
    delete &cache;
  }
  list.clear();
  DBUG_ASSERT(list.empty());
}


static
int online_alter_end_trans(Online_alter_cache_list &cache_list, THD *thd,
                           bool is_ending_transaction, bool commit)
{
  DBUG_ENTER("online_alter_end_trans");
  int error= 0;

  if (cache_list.empty())
    DBUG_RETURN(0);

  for (auto &cache: cache_list)
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
        if (!is_ending_transaction)
          cache.reset();
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
      break;
    }
  }

  if (is_ending_transaction)
    cleanup_cache_list(cache_list);

  DBUG_RETURN(error);
}

void cleanup_tables(THD *thd)
{
  for (TABLE *table= thd->open_tables; table; table= table->next)
    table->online_alter_cache= NULL;
}

static
int online_alter_savepoint_set(handlerton *hton, THD *thd, sv_id_t sv_id)
{
  DBUG_ENTER("binlog_online_alter_savepoint");
  auto &cache_list= get_cache_list(hton, thd);
  if (cache_list.empty())
    DBUG_RETURN(0);

  for (auto &cache: cache_list)
  {
    if (cache.hton->savepoint_set == NULL)
      continue;

    auto *sv= new table_savepoint(sv_id, cache.get_byte_position());
    if(unlikely(sv == NULL))
      DBUG_RETURN(1);
    cache.sv_list.push_front(*sv);
  }
  DBUG_RETURN(0);
}

static
int online_alter_savepoint_rollback(handlerton *hton, THD *thd, sv_id_t sv_id)
{
  DBUG_ENTER("online_alter_savepoint_rollback");

  auto &cache_list= get_cache_list(hton, thd);
  for (auto &cache: cache_list)
  {
    if (cache.hton->savepoint_set == NULL)
      continue;

    // There's no savepoint if it was set up before online table was modified.
    // In that case, restore to 0.
    my_off_t pos= cache.pop_sv_until(sv_id);

    cache.restore_savepoint(pos);
  }

  DBUG_RETURN(0);
}

static int online_alter_commit(handlerton *hton, THD *thd, bool all)
{
  int res;
  bool is_ending_transaction= ending_trans(thd, all);
  if (is_ending_transaction 
      && thd->transaction->xid_state.get_state_code() == XA_PREPARED)
  {
    res= hton->commit_by_xid(hton, thd->transaction->xid_state.get_xid());
    // cleanup was already done by prepare()
  }
  else
  {
    res= online_alter_end_trans(get_cache_list(hton, thd), thd,
                                is_ending_transaction, true);
    cleanup_tables(thd);
  }
  return res;
};

static int online_alter_rollback(handlerton *hton, THD *thd, bool all)
{
  int res;
  bool is_ending_transaction= ending_trans(thd, all);
  if (is_ending_transaction
      && thd->transaction->xid_state.get_state_code() == XA_PREPARED)
  {
    res= hton->rollback_by_xid(hton, thd->transaction->xid_state.get_xid());
    // cleanup was already done by prepare()
  }
  else
  {
    res= online_alter_end_trans(get_cache_list(hton, thd), thd,
                                is_ending_transaction, false);
    cleanup_tables(thd);
  }
  return res;
};

static int online_alter_prepare(handlerton *hton, THD *thd, bool all)
{
  auto &cache_list= get_cache_list(hton, thd);
  int res= 0;
  if (ending_trans(thd, all))
  {
    thd->transaction->xid_state.set_online_alter_cache(&cache_list);
    thd_set_ha_data(thd, hton, NULL);
  }
  else
  {
    res= online_alter_end_trans(cache_list, thd, false, true);
  }

  cleanup_tables(thd);
  return res;
};

static int online_alter_commit_by_xid(handlerton *hton, XID *x)
{
  auto *xid= static_cast<XA_data*>(x);
  if (likely(xid->online_alter_cache == NULL))
    return 1;
  int res= online_alter_end_trans(*xid->online_alter_cache, current_thd,
                                  true, true);
  delete xid->online_alter_cache;
  xid->online_alter_cache= NULL;
  return res;
};

static int online_alter_rollback_by_xid(handlerton *hton, XID *x)
{
  auto *xid= static_cast<XA_data*>(x);
  if (likely(xid->online_alter_cache == NULL))
    return 1;
  int res= online_alter_end_trans(*xid->online_alter_cache, current_thd,
                                  true, false);
  delete xid->online_alter_cache;
  xid->online_alter_cache= NULL;
  return res;
};

static int online_alter_close_connection(handlerton *hton, THD *thd)
{
  auto *cache_list= (Online_alter_cache_list*)thd_get_ha_data(thd, hton);

  DBUG_ASSERT(!cache_list || cache_list->empty());
  delete cache_list;
  thd_set_ha_data(thd, hton, NULL);
  return 0;
}


static int online_alter_log_init(void *p)
{
  online_alter_hton= (handlerton *)p;
  online_alter_hton->db_type= DB_TYPE_ONLINE_ALTER;
  online_alter_hton->savepoint_offset= 0;
  online_alter_hton->close_connection= online_alter_close_connection;

  online_alter_hton->savepoint_set= online_alter_savepoint_set;
  online_alter_hton->savepoint_rollback= online_alter_savepoint_rollback;
  online_alter_hton->savepoint_rollback_can_release_mdl=
          [](handlerton *hton, THD *thd){ return true; };

  online_alter_hton->commit= online_alter_commit;
  online_alter_hton->rollback= online_alter_rollback;


  online_alter_hton->recover= [](handlerton*, XID*, uint){ return 0; };
  online_alter_hton->prepare= online_alter_prepare;
  online_alter_hton->commit_by_xid= online_alter_commit_by_xid;
  online_alter_hton->rollback_by_xid= online_alter_rollback_by_xid;

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
