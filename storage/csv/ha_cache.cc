/* Copyright (C) 2020 MariaDB Corppration AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the
   Free Software Foundation, Inc.
   51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
*/

/*
  Implementation of ha_cache, ha_cache is an insert cache for ColumnStore to
  speed up inserts.

  The idea is that inserts are first stored in storage engine that is
  fast for inserts, like MyISAM or Aria, and in case of select,
  update, delete then the rows are first flushed to ColumnStore before
  the original requests is made.

  The table used for the cache is the original table name prefixed with #cache#
*/

/*
  TODO:
  - Add create flag to myisam_open that will ensure that on open and recovery
    it restores only as many rows as stored in the header.
    (Can be fast as we have number of rows and file size stored in the header)
  - Commit to the cache is now done per statement. Should be changed to be per
    transaction.  Should not be impossible to do as we only have to update
    key_file_length and data_file_length on commit.
  - On recovery, check all #cache# tables to see if the last stored commit
    is already in ColumnStore. If yes, truncate the cache.

  Things to consider:

    Current implementation is doing a syncronization tables as part of
    thd->get_status(), which is the function to be called when server
    has got a lock of all used tables. This ensure that the row
    visibility is the same for all tables.
    The disadvantage of this is that we have to always take a write lock
    for the cache table. In case of read transactions, this lock is released
    in free_locks() as soon as we get the table lock.
    Another alternative would be to assume that if there are no cached rows
    during the call to 'store_lock', then we can ignore any new rows added.
    This would allows us to avoid write locks for the cached table, except
    for inserts or if there are rows in the cache. The disadvanteg would be
    that we would not see any rows inserted while we are trying to get the
    lock.
*/

#define MYSQL_SERVER 1                          // We need access to THD
#include <my_global.h>
#include "sql_plugin.h"
#include "../maria/maria_def.h"
#include "sql_priv.h"
#include "sql_table.h"                          // tablename_to_filename
#include "sql_class.h"                          // THD
#include "ha_cache.h"

#define CACHE_PREFIX "#cache#"

static handlerton *derived_hton;
static my_bool (*original_get_status)(void*, my_bool);

my_bool get_status_and_flush_cache(void *param,
                                   my_bool concurrent_insert);

/*
  Create a name for the cache table
*/

static void create_cache_name(char *to, const char *name)
{
  uint dir_length= dirname_length(name);
  to= strnmov(to, name, dir_length);
  strxmov(to, CACHE_PREFIX, name+ dir_length, NullS);
}

/*****************************************************************************
 THR_LOCK wrapper functions

  The idea of these is to highjack 'THR_LOCK->get_status() so that if this
  is called in a non-insert context then we will flush the cache
*****************************************************************************/

/*
  First call to get_status() will flush the cache if the command is not an
  insert
*/

my_bool get_status_and_flush_cache(void *param,
                                   my_bool concurrent_insert)
{
  ha_cache *cache= (ha_cache*) param;
  int error;
  enum_sql_command sql_command= cache->table->in_use->lex->sql_command;
  cache->insert_command= (sql_command == SQLCOM_INSERT &&
                          sql_command == SQLCOM_LOAD);
  /*
    Call first the original Aria get_status function
    All Aria get_status functions takes Maria handler as the parameter
  */
  if (cache->share->org_lock.get_status)
    (*cache->share->org_lock.get_status)(&cache->cache_handler->file,
                                         concurrent_insert);

  /* If first get_status() call for this table, flush cache if needed */
  if (!cache->lock_counter++)
  {
    if (!cache->insert_command && cache->rows_cached())
    {
      if ((error= cache->flush_insert_cache()))
      {
        my_error(error, MYF(MY_WME | ME_FATAL),
                 "Got error while trying to flush insert cache: %d",
                 my_errno);
        return(1);
      }
    }
  }
  if (!cache->insert_command)
    cache->free_locks();

  return (0);
}

/* Pass through functions for all the THR_LOCK virtual functions */

static my_bool cache_start_trans(void* param)
{
  ha_cache *cache= (ha_cache*) param;
  return (*cache->share->org_lock.start_trans)(cache->cache_handler->file);
}

static void cache_copy_status(void* to, void *from)
{
  ha_cache *to_cache= (ha_cache*) to, *from_cache= (ha_cache*) from;
  (*to_cache->share->org_lock.copy_status)(to_cache->cache_handler->file,
                                           from_cache->cache_handler->file);
}

static void cache_update_status(void* param)
{
  ha_cache *cache= (ha_cache*) param;
  (*cache->share->org_lock.update_status)(cache->cache_handler->file);
}

static void cache_restore_status(void *param)
{
  ha_cache *cache= (ha_cache*) param;
  (*cache->share->org_lock.restore_status)(cache->cache_handler->file);
}

static my_bool cache_check_status(void *param)
{
  ha_cache *cache= (ha_cache*) param;
  return (*cache->share->org_lock.check_status)(cache->cache_handler->file);
}

/*****************************************************************************
 ha_cache_share functions (Common storage for an open cache file)
*****************************************************************************/

static ha_cache_share *cache_share_list= 0;
static PSI_mutex_key key_LOCK_cache_share;
static PSI_mutex_info all_mutexes[]=
{
  { &key_LOCK_cache_share, "LOCK_cache_share", PSI_FLAG_GLOBAL},
};
static mysql_mutex_t LOCK_cache_share;

/*
  Find or create a share
*/

ha_cache_share *find_cache_share(const char *name)
{
  ha_cache_share *pos, *share;
  mysql_mutex_lock(&LOCK_cache_share);
  for (pos= cache_share_list; pos; pos= pos->next)
  {
    if (!strcmp(pos->name, name))
    {
      mysql_mutex_unlock(&LOCK_cache_share);
      return(pos);
    }
  }
  if (!(share= (ha_cache_share*) my_malloc(PSI_NOT_INSTRUMENTED,
                                           sizeof(*share) + strlen(name)+1,
                                           MYF(MY_FAE))))
  {
    mysql_mutex_unlock(&LOCK_cache_share);
    return 0;
  }
  share->name= (char*) (share+1);
  share->open_count= 1;
  strmov((char*) share->name, name);
  share->next= cache_share_list;
  cache_share_list= share;
  mysql_mutex_unlock(&LOCK_cache_share);
  return share;
}


/*
  Decrement open counter and free share if there is no more users
*/

void ha_cache_share::close()
{
  ha_cache_share *pos;
  mysql_mutex_lock(&LOCK_cache_share);
  if (!--open_count)
  {
    ha_cache_share **prev= &cache_share_list;
    for ( ;  (pos= *prev) != this; prev= &pos->next)
      ;
    *prev= next;
    my_free(this);
  }
  mysql_mutex_unlock(&LOCK_cache_share);
}


/*****************************************************************************
 ha_cache handler functions
*****************************************************************************/

ha_cache::ha_cache(handlerton *hton, TABLE_SHARE *table_arg, MEM_ROOT *mem_root)
  :ha_tina(derived_hton, table_arg)
{
  cache_handler= new (mem_root) ha_maria(maria_hton, table_arg);
  share= 0;
  lock_counter= 0;
}


ha_cache::~ha_cache()
{
  delete cache_handler;
}


/*
  The following functions duplicates calls to derived handler and
  cache handler
*/

int ha_cache::create(const char *name, TABLE *table_arg,
                     HA_CREATE_INFO *ha_create_info)
{
  int error;
  char cache_name[FN_REFLEN+8];
  DBUG_ENTER("ha_cache::create");

  create_cache_name(cache_name, name);
  {
    /* Create a cached table */
    ha_choice save_transactional= ha_create_info->transactional;
    row_type save_row_type=       ha_create_info->row_type;
    ha_create_info->transactional= HA_CHOICE_NO;
    ha_create_info->row_type=      ROW_TYPE_DYNAMIC;

    if ((error= cache_handler->create(cache_name, table_arg, ha_create_info)))
      DBUG_RETURN(error);
    ha_create_info->transactional= save_transactional;
    ha_create_info->row_type=      save_row_type;
  }

  /* Create the real table in ColumnStore */
  if ((error= parent::create(name, table_arg, ha_create_info)))
  {
    cache_handler->delete_table(cache_name);
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}


int ha_cache::open(const char *name, int mode, uint open_flags)
{
  int error;
  char cache_name[FN_REFLEN+8];
  DBUG_ENTER("ha_cache::open");

  /* Copy table object to cache_handler */
  cache_handler->change_table_ptr(table, table->s);

  create_cache_name(cache_name, name);
  if ((error= cache_handler->open(cache_name, mode, open_flags)))
    DBUG_RETURN(error);

  if (!(share= find_cache_share(name)))
  {
    cache_handler->close();
    DBUG_RETURN(ER_OUTOFMEMORY);
  }

  /* Fix lock so that it goes through get_status_and_flush() */
  THR_LOCK *lock= &cache_handler->file->s->lock;
  if (lock->get_status != &get_status_and_flush_cache)
  {
    mysql_mutex_lock(&cache_handler->file->s->intern_lock);
    if (lock->get_status != &get_status_and_flush_cache)
    {
      /* Remember original lock. Used by the THR_lock cache functions */
      share->org_lock= lock[0];
      if (lock->start_trans)
        lock->start_trans=    &cache_start_trans;
      if (lock->copy_status)
        lock->copy_status=    &cache_copy_status;
      if (lock->update_status)
        lock->update_status=  &cache_update_status;
      if (lock->restore_status)
        lock->restore_status= &cache_restore_status;
      if (lock->check_status)
        lock->check_status=   &cache_check_status;
      if (lock->restore_status)
        lock->restore_status=  &cache_restore_status;
      lock->get_status=       &get_status_and_flush_cache;
    }
    mysql_mutex_unlock(&cache_handler->file->s->intern_lock);
  }
  cache_handler->file->lock.status_param= (void*) this;

  if ((error= parent::open(name, mode, open_flags)))
  {
    cache_handler->close();
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}


int ha_cache::close()
{
  int error, error2;
  ha_cache_share *org_share= share;
  DBUG_ENTER("ha_cache::close()");
  error= cache_handler->close();
  if ((error2= parent::close()))
    error= error2;
  if (org_share)
    org_share->close();
  DBUG_RETURN(error);
}


/*
   Handling locking of the tables. In case of INSERT we have to lock both
   the cache handler and main table. If not, we only lock the main table
*/

uint ha_cache::lock_count(void) const
{
  /*
    If we are doing an insert or if we want to flush the cache, we have to lock
    both MyISAM table and normal table.
  */
  return 2;
}

/**
   Store locks for the Aria table and ColumnStore table
*/

THR_LOCK_DATA **ha_cache::store_lock(THD *thd,
                                     THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type)
{
  to= cache_handler->store_lock(thd, to, TL_WRITE);
  return parent::store_lock(thd, to, lock_type);
}


/**
   Do external locking of the tables
*/

int ha_cache::external_lock(THD *thd, int lock_type)
{
  int error;
  DBUG_ENTER("ha_cache::external_lock");

  /*
    Reset lock_counter. This is ok as external_lock() is guaranteed to be
    called before first get_status()
  */
  lock_counter= 0;

  if (lock_type == F_UNLCK)
  {
    int error2;
    error= cache_handler->external_lock(thd, lock_type);
    if ((error2= parent::external_lock(thd, lock_type)))
      error= error2;
    DBUG_RETURN(error);
  }

  /* Lock first with write lock to be able to do insert or flush table */
  original_lock_type= lock_type;
  lock_type= F_WRLCK;
  if ((error= cache_handler->external_lock(thd, lock_type)))
    DBUG_RETURN(error);
  if ((error= parent::external_lock(thd, lock_type)))
  {
    error= cache_handler->external_lock(thd, F_UNLCK);
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}


int ha_cache::delete_table(const char *name)
{
  int error, error2;
  char cache_name[FN_REFLEN+8];
  DBUG_ENTER("ha_cache::delete_table");

  create_cache_name(cache_name, name);
  error= cache_handler->delete_table(cache_name);
  if ((error2= parent::delete_table(name)))
    error= error2;
  DBUG_RETURN(error);
}


int ha_cache::rename_table(const char *from, const char *to)
{
  int error;
  char cache_from[FN_REFLEN+8], cache_to[FN_REFLEN+8];
  DBUG_ENTER("ha_cache::rename_table");

  create_cache_name(cache_from, from);
  create_cache_name(cache_to, to);
  if ((error= cache_handler->rename_table(cache_from, cache_to)))
    DBUG_RETURN(error);

  if ((error= parent::rename_table(from, to)))
  {
    cache_handler->rename_table(cache_to, cache_from);
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}


int ha_cache::delete_all_rows(void)
{
  int error,error2;
  DBUG_ENTER("ha_cache::delete_all_rows");

  error= cache_handler->delete_all_rows();
  if ((error2= parent::delete_all_rows()))
    error= error2;
  DBUG_RETURN(error);
}

bool ha_cache::is_crashed() const
{
  return (cache_handler->is_crashed() ||
          parent::is_crashed());
}

/**
   After a crash, repair will be run on next open.

   There are two cases when repair is run:
   1) Automatically on open if the table is crashed
   2) When the user explicitely runs repair

   In the case of 1) we don't want to run repair on both tables as
   the repair can be a slow process. Instead we only run repair
   on the crashed tables. If not tables are marked crashed, we
   run repair on both tables.

   Repair on the cache table will delete the part of the cache that was
   not committed.

   key_file_length and data_file_length are updated last for a statement.
   When these are updated, we threat the cache as committed
*/

int ha_cache::repair(THD *thd, HA_CHECK_OPT *check_opt)
{
  int error= 0, error2;
  int something_crashed= is_crashed();
  DBUG_ENTER("ha_cache::repair");

  if (cache_handler->is_crashed() || !something_crashed)
  {
    /* Delete everything that was not already committed */
    mysql_file_chsize(cache_handler->file->dfile.file,
                      cache_handler->file->s->state.state.key_file_length,
                      0, MYF(MY_WME));
    mysql_file_chsize(cache_handler->file->s->kfile.file,
                      cache_handler->file->s->state.state.data_file_length,
                      0, MYF(MY_WME));
    error= cache_handler->repair(thd, check_opt);
  }
  if (parent::is_crashed() || !something_crashed)
    if ((error2= parent::repair(thd, check_opt)))
      error= error2;

  DBUG_RETURN(error);
}


/**
   Write to cache handler or main table
*/
int ha_cache::write_row(const uchar *buf)
{
  if (insert_command)
    return cache_handler->write_row(buf);
  return parent::write_row(buf);
}


void ha_cache::start_bulk_insert(ha_rows rows, uint flags)
{
  if (insert_command)
  {
    bzero(&cache_handler->copy_info, sizeof(cache_handler->copy_info));
    return cache_handler->start_bulk_insert(rows, flags);
  }
  return parent::start_bulk_insert(rows, flags);
}


int ha_cache::end_bulk_insert()
{
  if (insert_command)
    return cache_handler->end_bulk_insert();
  return parent::end_bulk_insert();
}

/******************************************************************************
 Plugin code
******************************************************************************/

static handler *ha_cache_create_handler(handlerton *hton,
                                        TABLE_SHARE *table,
                                        MEM_ROOT *mem_root)
{
  return new (mem_root) ha_cache(hton, table, mem_root);
}

static plugin_ref plugin;

static int ha_cache_init(void *p)
{
  handlerton *cache_hton;
  int error;
  uint count;

  cache_hton= (handlerton *) p;
  cache_hton->create= ha_cache_create_handler;
  cache_hton->panic= 0;
  cache_hton->flags= HTON_NO_PARTITION;

  count= sizeof(all_mutexes)/sizeof(all_mutexes[0]);
  mysql_mutex_register("ha_cache", all_mutexes, count);
  mysql_mutex_init(key_LOCK_cache_share, &LOCK_cache_share, MY_MUTEX_INIT_FAST);

  {
    LEX_CSTRING name= { STRING_WITH_LEN("CSV") };
    plugin= ha_resolve_by_name(0, &name, 0);
    derived_hton= plugin_hton(plugin);
    error= derived_hton == 0;                   // Engine must exists!
    if (error)
      my_error(HA_ERR_INITIALIZATION, MYF(0),
               "Could not find storage engine %s", name.str);
  }
  return error;
}

static int ha_cache_deinit(void *p)
{
  if (plugin)
  {
    plugin_unlock(0, plugin);
    plugin= 0;
  }
  mysql_mutex_destroy(&LOCK_cache_share);
  return 0;
}

struct st_mysql_storage_engine ha_cache_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(cache)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &ha_cache_storage_engine,
  "Columnstore_cache",
  "MariaDB Corporation AB",
  "Insert cache for ColumnStore",
  PLUGIN_LICENSE_GPL,
  ha_cache_init,                /* Plugin Init */
  ha_cache_deinit,              /* Plugin Deinit */
  0x0100,                       /* 1.0 */
  NULL,   		        /* status variables */
  NULL,                         /* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_ALPHA /* maturity         */
}
maria_declare_plugin_end;


/******************************************************************************
Implementation of write cache
******************************************************************************/

bool ha_cache::rows_cached()
{
  return cache_handler->file->state->records != 0;
}


/* Free write locks if this was not an insert */

void ha_cache::free_locks()
{
  /* We don't need to lock cache_handler anymore as it's already flushed */

  mysql_mutex_unlock(&cache_handler->file->lock.lock->mutex);
  thr_unlock(&cache_handler->file->lock, 0);

  /* Restart transaction for columnstore table */
  if (original_lock_type != F_WRLCK)
  {
    parent::external_lock(table->in_use, F_UNLCK);
    parent::external_lock(table->in_use, original_lock_type);
  }

  /* Needed as we are going back to end of thr_lock() */
  mysql_mutex_lock(&cache_handler->file->lock.lock->mutex);
}

/**
   Copy data from cache to ColumnStore

   Both tables are locked. The from table has also an exclusive lock to
   ensure that no one is inserting data to it while we are reading it.
*/

int ha_cache::flush_insert_cache()
{
  int error, error2;
  ha_maria *from= cache_handler;
  parent    *to= this;
  uchar *record= to->table->record[0];
  DBUG_ENTER("flush_insert_cache");

  to->start_bulk_insert(from->file->state->records, 0);
  from->rnd_init(1);
  while (!(error= from->rnd_next(record)))
  {
    if ((error= to->write_row(record)))
      goto end;
  }
  if (error == HA_ERR_END_OF_FILE)
    error= 0;

end:
  from->rnd_end();
  if ((error2= to->end_bulk_insert()) && !error)
    error= error2;

  if (!error)
  {
    if (to->ht->commit)
      error= to->ht->commit(to->ht, table->in_use, 1);
  }
  else
  {
    /* We can ignore the rollback error as we already have some other errors */
    if (to->ht->rollback)
      to->ht->rollback(to->ht, table->in_use, 1);
  }

  if (!error)
  {
    /*
      Everything when fine, delete all rows from the cache and allow others
      to use it.
    */
    from->delete_all_rows();

    /*
      This was not an insert command, so we can delete the thr lock
      (We are not going to use the insert cache for this statement anymore)
    */
    free_locks();
  }
  DBUG_RETURN(error);
}
