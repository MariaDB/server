/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2011 Monty Program Ab
   Copyright (C) 2013 Sergey Vojtovich and MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file
  Table definition cache and table cache implementation.

  Table definition cache actions:
  - add new TABLE_SHARE object to cache (tdc_acquire_share())
  - acquire TABLE_SHARE object from cache (tdc_acquire_share())
  - release TABLE_SHARE object to cache (tdc_release_share())
  - purge unused TABLE_SHARE objects from cache (tdc_purge())
  - remove TABLE_SHARE object from cache (tdc_remove_table())
  - get number of TABLE_SHARE objects in cache (tdc_records())

  Table cache actions:
  - add new TABLE object to cache (tc_add_table())
  - acquire TABLE object from cache (tc_acquire_table())
  - release TABLE object to cache (tc_release_table())
  - purge unused TABLE objects from cache (tc_purge())
  - purge unused TABLE objects of a table from cache (tdc_remove_table())
  - get number of TABLE objects in cache (tc_records())

  Dependencies:
  - intern_close_table(): frees TABLE object
  - kill_delayed_threads_for_table()
  - close_cached_tables(): flush tables on shutdown
  - alloc_table_share()
  - free_table_share()

  Table cache invariants:
  - TABLE_SHARE::free_tables shall not contain objects with TABLE::in_use != 0
  - TABLE_SHARE::free_tables shall not receive new objects if
    TABLE_SHARE::tdc.flushed is true
*/

#include "my_global.h"
#include "lf.h"
#include "table.h"
#include "sql_base.h"


/** Configuration. */
ulong tdc_size; /**< Table definition cache threshold for LRU eviction. */
ulong tc_size; /**< Table cache threshold for LRU eviction. */

/** Data collections. */
static LF_HASH tdc_hash; /**< Collection of TABLE_SHARE objects. */
/** Collection of unused TABLE_SHARE objects. */
I_P_List <TDC_element,
          I_P_List_adapter<TDC_element, &TDC_element::next, &TDC_element::prev>,
          I_P_List_null_counter,
          I_P_List_fast_push_back<TDC_element> > unused_shares;

static int64 tdc_version;  /* Increments on each reload */
static bool tdc_inited;

static int32 tc_count; /**< Number of TABLE objects in table cache. */


/**
  Protects unused shares list.

  TDC_element::prev
  TDC_element::next
  unused_shares
*/

static mysql_mutex_t LOCK_unused_shares;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_unused_shares, key_TABLE_SHARE_LOCK_table_share;
static PSI_mutex_info all_tc_mutexes[]=
{
  { &key_LOCK_unused_shares, "LOCK_unused_shares", PSI_FLAG_GLOBAL },
  { &key_TABLE_SHARE_LOCK_table_share, "TABLE_SHARE::tdc.LOCK_table_share", 0 }
};

PSI_cond_key key_TABLE_SHARE_COND_release;
static PSI_cond_info all_tc_conds[]=
{
  { &key_TABLE_SHARE_COND_release, "TABLE_SHARE::tdc.COND_release", 0 }
};


static void init_tc_psi_keys(void)
{
  const char *category= "sql";
  int count;

  count= array_elements(all_tc_mutexes);
  mysql_mutex_register(category, all_tc_mutexes, count);

  count= array_elements(all_tc_conds);
  mysql_cond_register(category, all_tc_conds, count);
}
#endif


static int fix_thd_pins(THD *thd)
{
  return thd->tdc_hash_pins ? 0 :
         (thd->tdc_hash_pins= lf_hash_get_pins(&tdc_hash)) == 0;
}


/*
  Auxiliary routines for manipulating with per-share all/unused lists
  and tc_count counter.
  Responsible for preserving invariants between those lists, counter
  and TABLE::in_use member.
  In fact those routines implement sort of implicit table cache as
  part of table definition cache.
*/


/**
  Get number of TABLE objects (used and unused) in table cache.
*/

uint tc_records(void)
{
  return my_atomic_load32_explicit(&tc_count, MY_MEMORY_ORDER_RELAXED);
}


/**
  Remove TABLE object from table cache.

  - decrement tc_count
  - remove object from TABLE_SHARE::tdc.all_tables
*/

static void tc_remove_table(TABLE *table)
{
  my_atomic_add32_explicit(&tc_count, -1, MY_MEMORY_ORDER_RELAXED);
  table->s->tdc->all_tables.remove(table);
}


/**
  Free all unused TABLE objects.

  While locked:
  - remove unused objects from TABLE_SHARE::tdc.free_tables and
    TABLE_SHARE::tdc.all_tables
  - decrement tc_count

  While unlocked:
  - free resources related to unused objects

  @note This is called by 'handle_manager' when one wants to
        periodicly flush all not used tables.
*/

struct tc_purge_arg
{
  TDC_element::TABLE_list purge_tables;
  bool mark_flushed;
};


static my_bool tc_purge_callback(TDC_element *element, tc_purge_arg *arg)
{
  TABLE *table;

  mysql_mutex_lock(&element->LOCK_table_share);
  element->wait_for_mdl_deadlock_detector();
  if (arg->mark_flushed)
    element->flushed= true;
  while ((table= element->free_tables.pop_front()))
  {
    tc_remove_table(table);
    arg->purge_tables.push_front(table);
  }
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


void tc_purge(bool mark_flushed)
{
  tc_purge_arg argument;
  TABLE *table;

  argument.mark_flushed= mark_flushed;
  tdc_iterate(0, (my_hash_walk_action) tc_purge_callback, &argument);
  while ((table= argument.purge_tables.pop_front()))
    intern_close_table(table);
}


/**
  Add new TABLE object to table cache.

  @pre TABLE object is used by caller.

  Added object cannot be evicted or acquired.

  While locked:
  - add object to TABLE_SHARE::tdc.all_tables
  - increment tc_count
  - evict LRU object from table cache if we reached threshold

  While unlocked:
  - free evicted object
*/

struct tc_add_table_arg
{
  char key[MAX_DBKEY_LENGTH];
  uint key_length;
  ulonglong purge_time;
};


my_bool tc_add_table_callback(TDC_element *element, tc_add_table_arg *arg)
{
  TABLE *table;

  mysql_mutex_lock(&element->LOCK_table_share);
  if ((table= element->free_tables_back()) && table->tc_time < arg->purge_time)
  {
    memcpy(arg->key, element->m_key, element->m_key_length);
    arg->key_length= element->m_key_length;
    arg->purge_time= table->tc_time;
  }
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


void tc_add_table(THD *thd, TABLE *table)
{
  bool need_purge;
  DBUG_ASSERT(table->in_use == thd);
  mysql_mutex_lock(&table->s->tdc->LOCK_table_share);
  table->s->tdc->wait_for_mdl_deadlock_detector();
  table->s->tdc->all_tables.push_front(table);
  mysql_mutex_unlock(&table->s->tdc->LOCK_table_share);

  /* If we have too many TABLE instances around, try to get rid of them */
  need_purge= my_atomic_add32_explicit(&tc_count, 1, MY_MEMORY_ORDER_RELAXED) >=
              (int32) tc_size;

  if (need_purge)
  {
    tc_add_table_arg argument;
    argument.purge_time= ULONGLONG_MAX;
    tdc_iterate(thd, (my_hash_walk_action) tc_add_table_callback, &argument);

    if (argument.purge_time != ULONGLONG_MAX)
    {
      TDC_element *element= (TDC_element*) lf_hash_search(&tdc_hash,
                                                          thd->tdc_hash_pins,
                                                          argument.key,
                                                          argument.key_length);
      if (element)
      {
        TABLE *entry;
        mysql_mutex_lock(&element->LOCK_table_share);
        lf_hash_search_unpin(thd->tdc_hash_pins);
        element->wait_for_mdl_deadlock_detector();

        /*
          It may happen that oldest table was acquired meanwhile. In this case
          just go ahead, number of objects in table cache will normalize
          eventually.
        */
        if ((entry= element->free_tables_back()) &&
            entry->tc_time == argument.purge_time)
        {
          element->free_tables.remove(entry);
          tc_remove_table(entry);
          mysql_mutex_unlock(&element->LOCK_table_share);
          intern_close_table(entry);
        }
        else
          mysql_mutex_unlock(&element->LOCK_table_share);
      }
    }
  }
}


/**
  Release TABLE object to table cache.

  @pre object is used by caller.

  Released object may be evicted or acquired again.

  While locked:
  - if object is marked for purge, decrement tc_count
  - add object to TABLE_SHARE::tdc.free_tables
  - evict LRU object from table cache if we reached threshold

  While unlocked:
  - mark object not in use by any thread
  - free evicted/purged object

  @note Another thread may mark share for purge any moment (even
  after version check). It means to-be-purged object may go to
  unused lists. This other thread is expected to call tc_purge(),
  which is synchronized with us on TABLE_SHARE::tdc.LOCK_table_share.

  @return
    @retval true  object purged
    @retval false object released
*/

bool tc_release_table(TABLE *table)
{
  DBUG_ASSERT(table->in_use);
  DBUG_ASSERT(table->file);

  if (table->needs_reopen() || tc_records() > tc_size)
  {
    mysql_mutex_lock(&table->s->tdc->LOCK_table_share);
    goto purge;
  }

  table->tc_time= my_interval_timer();

  mysql_mutex_lock(&table->s->tdc->LOCK_table_share);
  if (table->s->tdc->flushed)
    goto purge;
  /*
    in_use doesn't really need mutex protection, but must be reset after
    checking tdc.flushed and before this table appears in free_tables.
    Resetting in_use is needed only for print_cached_tables() and
    list_open_tables().
  */
  table->in_use= 0;
  /* Add table to the list of unused TABLE objects for this share. */
  table->s->tdc->free_tables.push_front(table);
  mysql_mutex_unlock(&table->s->tdc->LOCK_table_share);
  return false;

purge:
  table->s->tdc->wait_for_mdl_deadlock_detector();
  tc_remove_table(table);
  mysql_mutex_unlock(&table->s->tdc->LOCK_table_share);
  table->in_use= 0;
  intern_close_table(table);
  return true;
}


/**
  Delete share from hash and free share object.
*/

static void tdc_delete_share_from_hash(TDC_element *element)
{
  THD *thd= current_thd;
  LF_PINS *pins;
  TABLE_SHARE *share;
  DBUG_ENTER("tdc_delete_share_from_hash");

  mysql_mutex_assert_owner(&element->LOCK_table_share);
  share= element->share;
  DBUG_ASSERT(share);
  element->share= 0;
  PSI_CALL_release_table_share(share->m_psi);
  share->m_psi= 0;

  if (!element->m_flush_tickets.is_empty())
  {
    Wait_for_flush_list::Iterator it(element->m_flush_tickets);
    Wait_for_flush *ticket;
    while ((ticket= it++))
      (void) ticket->get_ctx()->m_wait.set_status(MDL_wait::GRANTED);

    do
    {
      mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
    } while (!element->m_flush_tickets.is_empty());
  }

  mysql_mutex_unlock(&element->LOCK_table_share);

  if (thd)
  {
    fix_thd_pins(thd);
    pins= thd->tdc_hash_pins;
  }
  else
    pins= lf_hash_get_pins(&tdc_hash);

  DBUG_ASSERT(pins); // What can we do about it?
  element->assert_clean_share();
  lf_hash_delete(&tdc_hash, pins, element->m_key, element->m_key_length);
  if (!thd)
    lf_hash_put_pins(pins);
  free_table_share(share);
  DBUG_VOID_RETURN;
}


/**
  Initialize table definition cache.
*/

void tdc_init(void)
{
  DBUG_ENTER("tdc_init");
#ifdef HAVE_PSI_INTERFACE
  init_tc_psi_keys();
#endif
  tdc_inited= true;
  mysql_mutex_init(key_LOCK_unused_shares, &LOCK_unused_shares,
                   MY_MUTEX_INIT_FAST);
  tdc_version= 1L;  /* Increments on each reload */
  lf_hash_init(&tdc_hash, sizeof(TDC_element), LF_HASH_UNIQUE, 0, 0,
               (my_hash_get_key) TDC_element::key,
               &my_charset_bin);
  tdc_hash.alloc.constructor= TDC_element::lf_alloc_constructor;
  tdc_hash.alloc.destructor= TDC_element::lf_alloc_destructor;
  tdc_hash.initializer= (lf_hash_initializer) TDC_element::lf_hash_initializer;
  DBUG_VOID_RETURN;
}


/**
  Notify table definition cache that process of shutting down server
  has started so it has to keep number of TABLE and TABLE_SHARE objects
  minimal in order to reduce number of references to pluggable engines.
*/

void tdc_start_shutdown(void)
{
  DBUG_ENTER("table_def_start_shutdown");
  if (tdc_inited)
  {
    /*
      Ensure that TABLE and TABLE_SHARE objects which are created for
      tables that are open during process of plugins' shutdown are
      immediately released. This keeps number of references to engine
      plugins minimal and allows shutdown to proceed smoothly.
    */
    tdc_size= 0;
    tc_size= 0;
    /* Free all cached but unused TABLEs and TABLE_SHAREs. */
    close_cached_tables(NULL, NULL, FALSE, LONG_TIMEOUT);
  }
  DBUG_VOID_RETURN;
}


/**
  Deinitialize table definition cache.
*/

void tdc_deinit(void)
{
  DBUG_ENTER("tdc_deinit");
  if (tdc_inited)
  {
    tdc_inited= false;
    lf_hash_destroy(&tdc_hash);
    mysql_mutex_destroy(&LOCK_unused_shares);
  }
  DBUG_VOID_RETURN;
}


/**
  Get number of cached table definitions.

  @return Number of cached table definitions
*/

ulong tdc_records(void)
{
  return my_atomic_load32_explicit(&tdc_hash.count, MY_MEMORY_ORDER_RELAXED);
}


void tdc_purge(bool all)
{
  DBUG_ENTER("tdc_purge");
  while (all || tdc_records() > tdc_size)
  {
    TDC_element *element;

    mysql_mutex_lock(&LOCK_unused_shares);
    if (!(element= unused_shares.pop_front()))
    {
      mysql_mutex_unlock(&LOCK_unused_shares);
      break;
    }

    /* Concurrent thread may start using share again, reset prev and next. */
    element->prev= 0;
    element->next= 0;
    mysql_mutex_lock(&element->LOCK_table_share);
    if (element->ref_count)
    {
      mysql_mutex_unlock(&element->LOCK_table_share);
      mysql_mutex_unlock(&LOCK_unused_shares);
      continue;
    }
    mysql_mutex_unlock(&LOCK_unused_shares);

    tdc_delete_share_from_hash(element);
  }
  DBUG_VOID_RETURN;
}


/**
  Lock table share.

  Find table share with given db.table_name in table definition cache. Return
  locked table share if found.

  Locked table share means:
  - table share is protected against removal from table definition cache
  - no other thread can acquire/release table share

  Caller is expected to unlock table share with tdc_unlock_share().

  @retval 0 Share not found
  @retval MY_ERRPTR OOM
  @retval ptr Pointer to locked table share
*/

TDC_element *tdc_lock_share(THD *thd, const char *db, const char *table_name)
{
  TDC_element *element;
  char key[MAX_DBKEY_LENGTH];

  DBUG_ENTER("tdc_lock_share");
  if (fix_thd_pins(thd))
    DBUG_RETURN((TDC_element*) MY_ERRPTR);

  element= (TDC_element *) lf_hash_search(&tdc_hash, thd->tdc_hash_pins,
                                          (uchar*) key,
                                          tdc_create_key(key, db, table_name));
  if (element)
  {
    mysql_mutex_lock(&element->LOCK_table_share);
    if (!element->share || element->share->error)
    {
      mysql_mutex_unlock(&element->LOCK_table_share);
      element= 0;
    }
    lf_hash_search_unpin(thd->tdc_hash_pins);
  }

  DBUG_RETURN(element);
}


/**
  Unlock share locked by tdc_lock_share().
*/

void tdc_unlock_share(TDC_element *element)
{
  DBUG_ENTER("tdc_unlock_share");
  mysql_mutex_unlock(&element->LOCK_table_share);
  DBUG_VOID_RETURN;
}


/*
  Get TABLE_SHARE for a table.

  tdc_acquire_share()
  thd                   Thread handle
  table_list            Table that should be opened
  key                   Table cache key
  key_length            Length of key
  flags                 operation: what to open table or view

  IMPLEMENTATION
    Get a table definition from the table definition cache.
    If it doesn't exist, create a new from the table definition file.

  RETURN
   0  Error
   #  Share for table
*/

TABLE_SHARE *tdc_acquire_share(THD *thd, TABLE_LIST *tl, uint flags,
                               TABLE **out_table)
{
  TABLE_SHARE *share;
  TDC_element *element;
  const char *key;
  uint key_length= get_table_def_key(tl, &key);
  my_hash_value_type hash_value= tl->mdl_request.key.tc_hash_value();
  bool was_unused;
  DBUG_ENTER("tdc_acquire_share");

  if (fix_thd_pins(thd))
    DBUG_RETURN(0);

retry:
  while (!(element= (TDC_element*) lf_hash_search_using_hash_value(&tdc_hash,
                    thd->tdc_hash_pins, hash_value, (uchar*) key, key_length)))
  {
    LEX_STRING tmp= { const_cast<char*>(key), key_length };
    int res= lf_hash_insert(&tdc_hash, thd->tdc_hash_pins, (uchar*) &tmp);

    if (res == -1)
      DBUG_RETURN(0);
    else if (res == 1)
      continue;

    element= (TDC_element*) lf_hash_search_using_hash_value(&tdc_hash,
             thd->tdc_hash_pins, hash_value, (uchar*) key, key_length);
    lf_hash_search_unpin(thd->tdc_hash_pins);
    DBUG_ASSERT(element);

    if (!(share= alloc_table_share(tl->db, tl->table_name, key, key_length)))
    {
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    /* note that tdc_acquire_share() *always* uses discovery */
    open_table_def(thd, share, flags | GTS_USE_DISCOVERY);

    if (share->error)
    {
      free_table_share(share);
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    mysql_mutex_lock(&element->LOCK_table_share);
    element->share= share;
    share->tdc= element;
    element->ref_count++;
    element->version= tdc_refresh_version();
    element->flushed= false;
    mysql_mutex_unlock(&element->LOCK_table_share);

    tdc_purge(false);
    if (out_table)
      *out_table= 0;
    share->m_psi= PSI_CALL_get_table_share(false, share);
    goto end;
  }

  /* cannot force discovery of a cached share */
  DBUG_ASSERT(!(flags & GTS_FORCE_DISCOVERY));

  if (out_table && (flags & GTS_TABLE))
  {
    if ((*out_table= element->acquire_table(thd)))
    {
      lf_hash_search_unpin(thd->tdc_hash_pins);
      DBUG_ASSERT(!(flags & GTS_NOLOCK));
      DBUG_ASSERT(element->share);
      DBUG_ASSERT(!element->share->error);
      DBUG_ASSERT(!element->share->is_view);
      DBUG_RETURN(element->share);
    }
  }

  mysql_mutex_lock(&element->LOCK_table_share);
  if (!(share= element->share))
  {
    mysql_mutex_unlock(&element->LOCK_table_share);
    lf_hash_search_unpin(thd->tdc_hash_pins);
    goto retry;
  }
  lf_hash_search_unpin(thd->tdc_hash_pins);

  /*
     We found an existing table definition. Return it if we didn't get
     an error when reading the table definition from file.
  */
  if (share->error)
  {
    open_table_error(share, share->error, share->open_errno);
    goto err;
  }

  if (share->is_view && !(flags & GTS_VIEW))
  {
    open_table_error(share, OPEN_FRM_NOT_A_TABLE, ENOENT);
    goto err;
  }
  if (!share->is_view && !(flags & GTS_TABLE))
  {
    open_table_error(share, OPEN_FRM_NOT_A_VIEW, ENOENT);
    goto err;
  }

  was_unused= !element->ref_count;
  element->ref_count++;
  mysql_mutex_unlock(&element->LOCK_table_share);
  if (was_unused)
  {
    mysql_mutex_lock(&LOCK_unused_shares);
    if (element->prev)
    {
      /*
        Share was not used before and it was in the old_unused_share list
        Unlink share from this list
      */
      DBUG_PRINT("info", ("Unlinking from not used list"));
      unused_shares.remove(element);
      element->next= 0;
      element->prev= 0;
    }
    mysql_mutex_unlock(&LOCK_unused_shares);
  }

end:
  DBUG_PRINT("exit", ("share: 0x%lx  ref_count: %u",
                      (ulong) share, share->tdc->ref_count));
  if (flags & GTS_NOLOCK)
  {
    tdc_release_share(share);
    /*
      if GTS_NOLOCK is requested, the returned share pointer cannot be used,
      the share it points to may go away any moment.
      But perhaps the caller is only interested to know whether a share or
      table existed?
      Let's return an invalid pointer here to catch dereferencing attempts.
    */
    share= (TABLE_SHARE*) 1;
  }
  DBUG_RETURN(share);

err:
  mysql_mutex_unlock(&element->LOCK_table_share);
  DBUG_RETURN(0);
}


/**
  Release table share acquired by tdc_acquire_share().
*/

void tdc_release_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_release_share");

  mysql_mutex_lock(&share->tdc->LOCK_table_share);
  DBUG_PRINT("enter",
             ("share: 0x%lx  table: %s.%s  ref_count: %u  version: %lu",
              (ulong) share, share->db.str, share->table_name.str,
              share->tdc->ref_count, share->tdc->version));
  DBUG_ASSERT(share->tdc->ref_count);

  if (share->tdc->ref_count > 1)
  {
    share->tdc->ref_count--;
    if (!share->is_view)
      mysql_cond_broadcast(&share->tdc->COND_release);
    mysql_mutex_unlock(&share->tdc->LOCK_table_share);
    DBUG_VOID_RETURN;
  }
  mysql_mutex_unlock(&share->tdc->LOCK_table_share);

  mysql_mutex_lock(&LOCK_unused_shares);
  mysql_mutex_lock(&share->tdc->LOCK_table_share);
  if (--share->tdc->ref_count)
  {
    mysql_mutex_unlock(&share->tdc->LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
  }
  if (share->tdc->flushed || tdc_records() > tdc_size)
  {
    mysql_mutex_unlock(&LOCK_unused_shares);
    tdc_delete_share_from_hash(share->tdc);
    DBUG_VOID_RETURN;
  }
  /* Link share last in used_table_share list */
  DBUG_PRINT("info", ("moving share to unused list"));
  DBUG_ASSERT(share->tdc->next == 0);
  unused_shares.push_back(share->tdc);
  mysql_mutex_unlock(&share->tdc->LOCK_table_share);
  mysql_mutex_unlock(&LOCK_unused_shares);
  DBUG_VOID_RETURN;
}


/**
   Remove all or some (depending on parameter) instances of TABLE and
   TABLE_SHARE from the table definition cache.

   @param  thd          Thread context
   @param  remove_type  Type of removal:
                        TDC_RT_REMOVE_ALL     - remove all TABLE instances and
                                                TABLE_SHARE instance. There
                                                should be no used TABLE objects
                                                and caller should have exclusive
                                                metadata lock on the table.
                        TDC_RT_REMOVE_NOT_OWN - remove all TABLE instances
                                                except those that belong to
                                                this thread. There should be
                                                no TABLE objects used by other
                                                threads and caller should have
                                                exclusive metadata lock on the
                                                table.
                        TDC_RT_REMOVE_UNUSED  - remove all unused TABLE
                                                instances (if there are no
                                                used instances will also
                                                remove TABLE_SHARE).
                        TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE -
                                                remove all TABLE instances
                                                except those that belong to
                                                this thread, but don't mark
                                                TABLE_SHARE as old. There
                                                should be no TABLE objects
                                                used by other threads and
                                                caller should have exclusive
                                                metadata lock on the table.
   @param  db           Name of database
   @param  table_name   Name of table
   @param  kill_delayed_threads     If TRUE, kill INSERT DELAYED threads

   @note It assumes that table instances are already not used by any
   (other) thread (this should be achieved by using meta-data locks).
*/

bool tdc_remove_table(THD *thd, enum_tdc_remove_table_type remove_type,
                      const char *db, const char *table_name,
                      bool kill_delayed_threads)
{
  I_P_List <TABLE, TABLE_share> purge_tables;
  TABLE *table;
  TDC_element *element;
  uint my_refs= 1;
  DBUG_ENTER("tdc_remove_table");
  DBUG_PRINT("enter",("name: %s  remove_type: %d", table_name, remove_type));

  DBUG_ASSERT(remove_type == TDC_RT_REMOVE_UNUSED ||
              thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));


  mysql_mutex_lock(&LOCK_unused_shares);
  if (!(element= tdc_lock_share(thd, db, table_name)))
  {
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_ASSERT(remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE);
    DBUG_RETURN(false);
  }

  DBUG_ASSERT(element != MY_ERRPTR); // What can we do about it?

  if (!element->ref_count)
  {
    if (element->prev)
    {
      unused_shares.remove(element);
      element->prev= 0;
      element->next= 0;
    }
    mysql_mutex_unlock(&LOCK_unused_shares);

    tdc_delete_share_from_hash(element);
    DBUG_RETURN(true);
  }
  mysql_mutex_unlock(&LOCK_unused_shares);

  element->ref_count++;

  element->wait_for_mdl_deadlock_detector();
  /*
    Mark share flushed in order to ensure that it gets
    automatically deleted once it is no longer referenced.

    Note that code in TABLE_SHARE::wait_for_old_version() assumes that
    marking share flushed is followed by purge of unused table
    shares.
  */
  if (remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE)
    element->flushed= true;

  while ((table= element->free_tables.pop_front()))
  {
    tc_remove_table(table);
    purge_tables.push_front(table);
  }
  if (kill_delayed_threads)
    kill_delayed_threads_for_table(element);

  if (remove_type == TDC_RT_REMOVE_NOT_OWN ||
      remove_type == TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE)
  {
    TDC_element::All_share_tables_list::Iterator it(element->all_tables);
    while ((table= it++))
    {
      my_refs++;
      DBUG_ASSERT(table->in_use == thd);
    }
  }
  DBUG_ASSERT(element->all_tables.is_empty() || remove_type != TDC_RT_REMOVE_ALL);
  mysql_mutex_unlock(&element->LOCK_table_share);

  while ((table= purge_tables.pop_front()))
    intern_close_table(table);

  if (remove_type != TDC_RT_REMOVE_UNUSED)
  {
    /*
      Even though current thread holds exclusive metadata lock on this share
      (asserted above), concurrent FLUSH TABLES threads may be in process of
      closing unused table instances belonging to this share. E.g.:
      thr1 (FLUSH TABLES): table= share->tdc.free_tables.pop_front();
      thr1 (FLUSH TABLES): share->tdc.all_tables.remove(table);
      thr2 (ALTER TABLE): tdc_remove_table();
      thr1 (FLUSH TABLES): intern_close_table(table);

      Current remove type assumes that all table instances (except for those
      that are owned by current thread) must be closed before
      thd_remove_table() returns. Wait for such tables now.

      intern_close_table() decrements ref_count and signals COND_release. When
      ref_count drops down to number of references owned by current thread
      waiting is completed.

      Unfortunately TABLE_SHARE::wait_for_old_version() cannot be used here
      because it waits for all table instances, whereas we have to wait only
      for those that are not owned by current thread.
    */
    mysql_mutex_lock(&element->LOCK_table_share);
    while (element->ref_count > my_refs)
      mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
    mysql_mutex_unlock(&element->LOCK_table_share);
  }

  tdc_release_share(element->share);

  DBUG_RETURN(true);
}


/**
  Check if table's share is being removed from the table definition
  cache and, if yes, wait until the flush is complete.

  @param thd             Thread context.
  @param table_list      Table which share should be checked.
  @param timeout         Timeout for waiting.
  @param deadlock_weight Weight of this wait for deadlock detector.

  @retval 0       Success. Share is up to date or has been flushed.
  @retval 1       Error (OOM, was killed, the wait resulted
                  in a deadlock or timeout). Reported.
*/

int tdc_wait_for_old_version(THD *thd, const char *db, const char *table_name,
                             ulong wait_timeout, uint deadlock_weight,
                             ulong refresh_version)
{
  TDC_element *element;

  if (!(element= tdc_lock_share(thd, db, table_name)))
    return FALSE;
  else if (element == MY_ERRPTR)
    return TRUE;
  else if (element->flushed && refresh_version > element->version)
  {
    struct timespec abstime;
    set_timespec(abstime, wait_timeout);
    return element->share->wait_for_old_version(thd, &abstime, deadlock_weight);
  }
  tdc_unlock_share(element);
  return FALSE;
}


ulong tdc_refresh_version(void)
{
  return my_atomic_load64_explicit(&tdc_version, MY_MEMORY_ORDER_RELAXED);
}


ulong tdc_increment_refresh_version(void)
{
  ulong v= my_atomic_add64_explicit(&tdc_version, 1, MY_MEMORY_ORDER_RELAXED);
  DBUG_PRINT("tcache", ("incremented global refresh_version to: %lu", v));
  return v + 1;
}


/**
  Iterate table definition cache.

  Object is protected against removal from table definition cache.

  @note Returned TABLE_SHARE is not guaranteed to be fully initialized:
  tdc_acquire_share() added new share, but didn't open it yet. If caller
  needs fully initializer share, it must lock table share mutex.
*/

struct eliminate_duplicates_arg
{
  HASH hash;
  MEM_ROOT root;
  my_hash_walk_action action;
  void *argument;
};


static uchar *eliminate_duplicates_get_key(const uchar *element, size_t *length,
                                       my_bool not_used __attribute__((unused)))
{
  LEX_STRING *key= (LEX_STRING *) element;
  *length= key->length;
  return (uchar *) key->str;
}


static my_bool eliminate_duplicates(TDC_element *element,
                                    eliminate_duplicates_arg *arg)
{
  LEX_STRING *key= (LEX_STRING *) alloc_root(&arg->root, sizeof(LEX_STRING));

  if (!key || !(key->str= (char*) memdup_root(&arg->root, element->m_key,
                                              element->m_key_length)))
    return TRUE;

  key->length= element->m_key_length;

  if (my_hash_insert(&arg->hash, (uchar *) key))
    return FALSE;

  return arg->action(element, arg->argument);
}


int tdc_iterate(THD *thd, my_hash_walk_action action, void *argument,
                bool no_dups)
{
  eliminate_duplicates_arg no_dups_argument;
  LF_PINS *pins;
  myf alloc_flags= 0;
  uint hash_flags= HASH_UNIQUE;
  int res;

  if (thd)
  {
    fix_thd_pins(thd);
    pins= thd->tdc_hash_pins;
    alloc_flags= MY_THREAD_SPECIFIC;
    hash_flags|= HASH_THREAD_SPECIFIC;
  }
  else
    pins= lf_hash_get_pins(&tdc_hash);

  if (!pins)
    return ER_OUTOFMEMORY;

  if (no_dups)
  {
    init_alloc_root(&no_dups_argument.root, 4096, 4096, MYF(alloc_flags));
    my_hash_init(&no_dups_argument.hash, &my_charset_bin, tdc_records(), 0, 0,
                 eliminate_duplicates_get_key, 0, hash_flags);
    no_dups_argument.action= action;
    no_dups_argument.argument= argument;
    action= (my_hash_walk_action) eliminate_duplicates;
    argument= &no_dups_argument;
  }

  res= lf_hash_iterate(&tdc_hash, pins, action, argument);

  if (!thd)
    lf_hash_put_pins(pins);

  if (no_dups)
  {
    my_hash_free(&no_dups_argument.hash);
    free_root(&no_dups_argument.root, MYF(0));
  }
  return res;
}
