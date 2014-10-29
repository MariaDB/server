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
#include "hash.h"
#include "table.h"
#include "sql_base.h"

/** Configuration. */
ulong tdc_size; /**< Table definition cache threshold for LRU eviction. */
ulong tc_size; /**< Table cache threshold for LRU eviction. */

/** Data collections. */
static HASH tdc_hash; /**< Collection of TABLE_SHARE objects. */
/** Collection of unused TABLE_SHARE objects. */
static TABLE_SHARE *oldest_unused_share, end_of_unused_share;

static int64 tdc_version;  /* Increments on each reload */
static int64 last_table_id;
static bool tdc_inited;

static int32 tc_count; /**< Number of TABLE objects in table cache. */


/**
  Protects unused shares list.

  TABLE_SHARE::tdc.prev
  TABLE_SHARE::tdc.next
  oldest_unused_share
  end_of_unused_share
*/

static mysql_mutex_t LOCK_unused_shares;
static mysql_rwlock_t LOCK_tdc; /**< Protects tdc_hash. */
my_atomic_rwlock_t LOCK_tdc_atomics; /**< Protects tdc_version. */

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_unused_shares, key_TABLE_SHARE_LOCK_table_share;
static PSI_mutex_info all_tc_mutexes[]=
{
  { &key_LOCK_unused_shares, "LOCK_unused_shares", PSI_FLAG_GLOBAL },
  { &key_TABLE_SHARE_LOCK_table_share, "TABLE_SHARE::tdc.LOCK_table_share", 0 }
};

static PSI_rwlock_key key_rwlock_LOCK_tdc;
static PSI_rwlock_info all_tc_rwlocks[]=
{
  { &key_rwlock_LOCK_tdc, "LOCK_tdc", PSI_FLAG_GLOBAL }
};


static PSI_cond_key key_TABLE_SHARE_COND_release;
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

  count= array_elements(all_tc_rwlocks);
  mysql_rwlock_register(category, all_tc_rwlocks, count);

  count= array_elements(all_tc_conds);
  mysql_cond_register(category, all_tc_conds, count);
}
#endif


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
  uint count;
  my_atomic_rwlock_rdlock(&LOCK_tdc_atomics);
  count= my_atomic_load32(&tc_count);
  my_atomic_rwlock_rdunlock(&LOCK_tdc_atomics);
  return count;
}


/**
  Remove TABLE object from table cache.

  - decrement tc_count
  - remove object from TABLE_SHARE::tdc.all_tables
*/

static void tc_remove_table(TABLE *table)
{
  my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
  my_atomic_add32(&tc_count, -1);
  my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  table->s->tdc.all_tables.remove(table);
}


/**
  Wait for MDL deadlock detector to complete traversing tdc.all_tables.

  Must be called before updating TABLE_SHARE::tdc.all_tables.
*/

static void tc_wait_for_mdl_deadlock_detector(TABLE_SHARE *share)
{
  while (share->tdc.all_tables_refs)
    mysql_cond_wait(&share->tdc.COND_release, &share->tdc.LOCK_table_share);
}


/**
  Get last element of tdc.free_tables.
*/

static TABLE *tc_free_tables_back(TABLE_SHARE *share)
{
  TABLE_SHARE::TABLE_list::Iterator it(share->tdc.free_tables);
  TABLE *entry, *last= 0;
   while ((entry= it++))
     last= entry;
  return last;
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

void tc_purge(bool mark_flushed)
{
  TABLE_SHARE *share;
  TABLE *table;
  TDC_iterator tdc_it;
  TABLE_SHARE::TABLE_list purge_tables;

  tdc_it.init();
  while ((share= tdc_it.next()))
  {
    mysql_mutex_lock(&share->tdc.LOCK_table_share);
    tc_wait_for_mdl_deadlock_detector(share);

    if (mark_flushed)
      share->tdc.flushed= true;
    while ((table= share->tdc.free_tables.pop_front()))
    {
      tc_remove_table(table);
      purge_tables.push_front(table);
    }
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  }
  tdc_it.deinit();

  while ((table= purge_tables.pop_front()))
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

void tc_add_table(THD *thd, TABLE *table)
{
  bool need_purge;
  DBUG_ASSERT(table->in_use == thd);
  mysql_mutex_lock(&table->s->tdc.LOCK_table_share);
  tc_wait_for_mdl_deadlock_detector(table->s);
  table->s->tdc.all_tables.push_front(table);
  mysql_mutex_unlock(&table->s->tdc.LOCK_table_share);

  /* If we have too many TABLE instances around, try to get rid of them */
  my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
  need_purge= my_atomic_add32(&tc_count, 1) >= (int32) tc_size;
  my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);

  if (need_purge)
  {
    TABLE_SHARE *purge_share= 0;
    TABLE_SHARE *share;
    TABLE *entry;
    ulonglong UNINIT_VAR(purge_time);
    TDC_iterator tdc_it;

    tdc_it.init();
    while ((share= tdc_it.next()))
    {
      mysql_mutex_lock(&share->tdc.LOCK_table_share);
      if ((entry= tc_free_tables_back(share)) &&
          (!purge_share || entry->tc_time < purge_time))
      {
          purge_share= share;
          purge_time= entry->tc_time;
      }
      mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    }

    if (purge_share)
    {
      mysql_mutex_lock(&purge_share->tdc.LOCK_table_share);
      tc_wait_for_mdl_deadlock_detector(purge_share);
      tdc_it.deinit();
      /*
        It may happen that oldest table was acquired meanwhile. In this case
        just go ahead, number of objects in table cache will normalize
        eventually.
      */
      if ((entry= tc_free_tables_back(purge_share)) &&
          entry->tc_time == purge_time)
      {
        entry->s->tdc.free_tables.remove(entry);
        tc_remove_table(entry);
        mysql_mutex_unlock(&purge_share->tdc.LOCK_table_share);
        intern_close_table(entry);
      }
      else
        mysql_mutex_unlock(&purge_share->tdc.LOCK_table_share);
    }
    else
      tdc_it.deinit();
  }
}


/**
  Acquire TABLE object from table cache.

  @pre share must be protected against removal.

  Acquired object cannot be evicted or acquired again.

  While locked:
  - pop object from TABLE_SHARE::tdc.free_tables

  While unlocked:
  - mark object used by thd

  @return TABLE object, or NULL if no unused objects.
*/

static TABLE *tc_acquire_table(THD *thd, TABLE_SHARE *share)
{
  TABLE *table;

  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  table= share->tdc.free_tables.pop_front();
  if (table)
  {
    DBUG_ASSERT(!table->in_use);
    table->in_use= thd;
    /* The ex-unused table must be fully functional. */
    DBUG_ASSERT(table->db_stat && table->file);
    /* The children must be detached from the table. */
    DBUG_ASSERT(!table->file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
  }
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  return table;
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
    mysql_mutex_lock(&table->s->tdc.LOCK_table_share);
    goto purge;
  }

  table->tc_time= my_interval_timer();

  mysql_mutex_lock(&table->s->tdc.LOCK_table_share);
  if (table->s->tdc.flushed)
    goto purge;
  /*
    in_use doesn't really need mutex protection, but must be reset after
    checking tdc.flushed and before this table appears in free_tables.
    Resetting in_use is needed only for print_cached_tables() and
    list_open_tables().
  */
  table->in_use= 0;
  /* Add table to the list of unused TABLE objects for this share. */
  table->s->tdc.free_tables.push_front(table);
  mysql_mutex_unlock(&table->s->tdc.LOCK_table_share);
  return false;

purge:
  tc_wait_for_mdl_deadlock_detector(table->s);
  tc_remove_table(table);
  mysql_mutex_unlock(&table->s->tdc.LOCK_table_share);
  table->in_use= 0;
  intern_close_table(table);
  return true;
}


extern "C" uchar *tdc_key(const uchar *record, size_t *length,
                          my_bool not_used __attribute__((unused)))
{
  TABLE_SHARE *entry= (TABLE_SHARE*) record;
  *length= entry->table_cache_key.length;
  return (uchar*) entry->table_cache_key.str;
}


/**
  Delete share from hash and free share object.

  @return
    @retval 0 Success
    @retval 1 Share is referenced
*/

static int tdc_delete_share_from_hash(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_delete_share_from_hash");
  mysql_rwlock_wrlock(&LOCK_tdc);
  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  if (--share->tdc.ref_count)
  {
    mysql_cond_broadcast(&share->tdc.COND_release);
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_rwlock_unlock(&LOCK_tdc);
    DBUG_RETURN(1);
  }
  my_hash_delete(&tdc_hash, (uchar*) share);
  /* Notify PFS early, while still locked. */
  PSI_CALL_release_table_share(share->m_psi);
  share->m_psi= 0;
  mysql_rwlock_unlock(&LOCK_tdc);

  if (share->tdc.m_flush_tickets.is_empty())
  {
    /* No threads are waiting for this share to be flushed, destroy it. */
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    free_table_share(share);
  }
  else
  {
    Wait_for_flush_list::Iterator it(share->tdc.m_flush_tickets);
    Wait_for_flush *ticket;
    while ((ticket= it++))
      (void) ticket->get_ctx()->m_wait.set_status(MDL_wait::GRANTED);
    /*
      If there are threads waiting for this share to be flushed,
      the last one to receive the notification will destroy the
      share. At this point the share is removed from the table
      definition cache, so is OK to proceed here without waiting
      for this thread to do the work.
    */
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  }
  DBUG_RETURN(0);
}


/**
  Initialize table definition cache.

  @retval  0  Success
  @retval !0  Error
*/

int tdc_init(void)
{
  DBUG_ENTER("tdc_init");
#ifdef HAVE_PSI_INTERFACE
  init_tc_psi_keys();
#endif
  tdc_inited= true;
  mysql_mutex_init(key_LOCK_unused_shares, &LOCK_unused_shares,
                   MY_MUTEX_INIT_FAST);
  mysql_rwlock_init(key_rwlock_LOCK_tdc, &LOCK_tdc);
  my_atomic_rwlock_init(&LOCK_tdc_atomics);
  oldest_unused_share= &end_of_unused_share;
  end_of_unused_share.tdc.prev= &oldest_unused_share;
  tdc_version= 1L;  /* Increments on each reload */
  DBUG_RETURN(my_hash_init(&tdc_hash, &my_charset_bin, tdc_size, 0, 0, tdc_key,
                           0, 0));
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
    my_hash_free(&tdc_hash);
    my_atomic_rwlock_destroy(&LOCK_tdc_atomics);
    mysql_rwlock_destroy(&LOCK_tdc);
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
  ulong records;
  DBUG_ENTER("tdc_records");
  mysql_rwlock_rdlock(&LOCK_tdc);
  records= tdc_hash.records;
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_RETURN(records);
}


void tdc_purge(bool all)
{
  DBUG_ENTER("tdc_purge");
  while (all || tdc_records() > tdc_size)
  {
    TABLE_SHARE *share;

    mysql_mutex_lock(&LOCK_unused_shares);
    if (!oldest_unused_share->tdc.next)
    {
      mysql_mutex_unlock(&LOCK_unused_shares);
      break;
    }

    share= oldest_unused_share;
    *share->tdc.prev= share->tdc.next;
    share->tdc.next->tdc.prev= share->tdc.prev;
    /* Concurrent thread may start using share again, reset prev and next. */
    share->tdc.prev= 0;
    share->tdc.next= 0;
    mysql_mutex_lock(&share->tdc.LOCK_table_share);
    share->tdc.ref_count++;
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);

    tdc_delete_share_from_hash(share);
  }
  DBUG_VOID_RETURN;
}


/**
  Prepeare table share for use with table definition cache.
*/

void tdc_init_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_init_share");
  mysql_mutex_init(key_TABLE_SHARE_LOCK_table_share,
                   &share->tdc.LOCK_table_share, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_TABLE_SHARE_COND_release, &share->tdc.COND_release, 0);
  share->tdc.m_flush_tickets.empty();
  share->tdc.all_tables.empty();
  share->tdc.free_tables.empty();
  tdc_assign_new_table_id(share);
  share->tdc.version= tdc_refresh_version();
  share->tdc.flushed= false;
  share->tdc.all_tables_refs= 0;
  DBUG_VOID_RETURN;
}


/**
  Release table definition cache specific resources of table share.
*/

void tdc_deinit_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_deinit_share");
  DBUG_ASSERT(share->tdc.ref_count == 0);
  DBUG_ASSERT(share->tdc.m_flush_tickets.is_empty());
  DBUG_ASSERT(share->tdc.all_tables.is_empty());
  DBUG_ASSERT(share->tdc.free_tables.is_empty());
  DBUG_ASSERT(share->tdc.all_tables_refs == 0);
  mysql_cond_destroy(&share->tdc.COND_release);
  mysql_mutex_destroy(&share->tdc.LOCK_table_share);
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

  @retval  0 Share not found
  @retval !0 Pointer to locked table share
*/

TABLE_SHARE *tdc_lock_share(const char *db, const char *table_name)
{
  char key[MAX_DBKEY_LENGTH];
  uint key_length;

  DBUG_ENTER("tdc_lock_share");
  key_length= tdc_create_key(key, db, table_name);

  mysql_rwlock_rdlock(&LOCK_tdc);
  TABLE_SHARE* share= (TABLE_SHARE*) my_hash_search(&tdc_hash,
                                                    (uchar*) key, key_length);
  if (share && !share->error)
    mysql_mutex_lock(&share->tdc.LOCK_table_share);
  else
    share= 0;
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_RETURN(share);
}


/**
  Unlock share locked by tdc_lock_share().
*/

void tdc_unlock_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_unlock_share");
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
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

TABLE_SHARE *tdc_acquire_share(THD *thd, const char *db, const char *table_name,
                               const char *key, uint key_length,
                               my_hash_value_type hash_value, uint flags,
                               TABLE **out_table)
{
  TABLE_SHARE *share;
  bool was_unused;
  DBUG_ENTER("tdc_acquire_share");

  mysql_rwlock_rdlock(&LOCK_tdc);
  share= (TABLE_SHARE*) my_hash_search_using_hash_value(&tdc_hash, hash_value,
                                                        (uchar*) key,
                                                        key_length);
  if (!share)
  {
    TABLE_SHARE *new_share;
    mysql_rwlock_unlock(&LOCK_tdc);

    if (!(new_share= alloc_table_share(db, table_name, key, key_length)))
      DBUG_RETURN(0);
    new_share->error= OPEN_FRM_OPEN_ERROR;

    mysql_rwlock_wrlock(&LOCK_tdc);
    share= (TABLE_SHARE*) my_hash_search_using_hash_value(&tdc_hash, hash_value,
                                                          (uchar*) key,
                                                          key_length);
    if (!share)
    {
      bool need_purge;

      share= new_share;
      mysql_mutex_lock(&share->tdc.LOCK_table_share);
      if (my_hash_insert(&tdc_hash, (uchar*) share))
      {
        mysql_mutex_unlock(&share->tdc.LOCK_table_share);
        mysql_rwlock_unlock(&LOCK_tdc);
        free_table_share(share);
        DBUG_RETURN(0);
      }
      need_purge= tdc_hash.records > tdc_size;
      mysql_rwlock_unlock(&LOCK_tdc);

      /* note that tdc_acquire_share() *always* uses discovery */
      open_table_def(thd, share, flags | GTS_USE_DISCOVERY);
      share->tdc.ref_count++;
      mysql_mutex_unlock(&share->tdc.LOCK_table_share);

      if (share->error)
      {
        tdc_delete_share_from_hash(share);
        DBUG_RETURN(0);
      }
      else if (need_purge)
        tdc_purge(false);
      if (out_table)
        *out_table= 0;
      share->m_psi= PSI_CALL_get_table_share(false, share);
      goto end;
    }
    free_table_share(new_share);
  }

  /* cannot force discovery of a cached share */
  DBUG_ASSERT(!(flags & GTS_FORCE_DISCOVERY));

  if (out_table && (flags & GTS_TABLE))
  {
    if ((*out_table= tc_acquire_table(thd, share)))
    {
      mysql_rwlock_unlock(&LOCK_tdc);
      DBUG_ASSERT(!(flags & GTS_NOLOCK));
      DBUG_ASSERT(!share->error);
      DBUG_ASSERT(!share->is_view);
      DBUG_RETURN(share);
    }
  }

  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  mysql_rwlock_unlock(&LOCK_tdc);

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

  was_unused= !share->tdc.ref_count;
  share->tdc.ref_count++;
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  if (was_unused)
  {
    mysql_mutex_lock(&LOCK_unused_shares);
    if (share->tdc.prev)
    {
      /*
        Share was not used before and it was in the old_unused_share list
        Unlink share from this list
      */
      DBUG_PRINT("info", ("Unlinking from not used list"));
      *share->tdc.prev= share->tdc.next;
      share->tdc.next->tdc.prev= share->tdc.prev;
      share->tdc.next= 0;
      share->tdc.prev= 0;
    }
    mysql_mutex_unlock(&LOCK_unused_shares);
  }

end:
  DBUG_PRINT("exit", ("share: 0x%lx  ref_count: %u",
                      (ulong) share, share->tdc.ref_count));
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
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  DBUG_RETURN(0);
}


/**
  Release table share acquired by tdc_acquire_share().
*/

void tdc_release_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_release_share");

  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  DBUG_PRINT("enter",
             ("share: 0x%lx  table: %s.%s  ref_count: %u  version: %lu",
              (ulong) share, share->db.str, share->table_name.str,
              share->tdc.ref_count, share->tdc.version));
  DBUG_ASSERT(share->tdc.ref_count);

  if (share->tdc.ref_count > 1)
  {
    share->tdc.ref_count--;
    if (!share->is_view)
      mysql_cond_broadcast(&share->tdc.COND_release);
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    DBUG_VOID_RETURN;
  }
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);

  mysql_mutex_lock(&LOCK_unused_shares);
  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  if (share->tdc.flushed)
  {
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    tdc_delete_share_from_hash(share);
    DBUG_VOID_RETURN;
  }
  if (--share->tdc.ref_count)
  {
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
  }
  /* Link share last in used_table_share list */
  DBUG_PRINT("info", ("moving share to unused list"));
  DBUG_ASSERT(share->tdc.next == 0);
  share->tdc.prev= end_of_unused_share.tdc.prev;
  *end_of_unused_share.tdc.prev= share;
  end_of_unused_share.tdc.prev= &share->tdc.next;
  share->tdc.next= &end_of_unused_share;
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  mysql_mutex_unlock(&LOCK_unused_shares);

  /* Delete the least used share to preserve LRU order. */
  tdc_purge(false);
  DBUG_VOID_RETURN;
}


static TABLE_SHARE *tdc_delete_share(const char *db, const char *table_name)
{
  TABLE_SHARE *share;
  DBUG_ENTER("tdc_delete_share");

  while ((share= tdc_lock_share(db, table_name)))
  {
    share->tdc.ref_count++;
    if (share->tdc.ref_count > 1)
    {
      tdc_unlock_share(share);
      DBUG_RETURN(share);
    }
    tdc_unlock_share(share);

    mysql_mutex_lock(&LOCK_unused_shares);
    if (share->tdc.prev)
    {
      *share->tdc.prev= share->tdc.next;
      share->tdc.next->tdc.prev= share->tdc.prev;
      /* Concurrent thread may start using share again, reset prev and next. */
      share->tdc.prev= 0;
      share->tdc.next= 0;
    }
    mysql_mutex_unlock(&LOCK_unused_shares);

    if (!tdc_delete_share_from_hash(share))
      break;
  }
  DBUG_RETURN(0);
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
  TABLE *table;
  TABLE_SHARE *share;
  bool found= false;
  DBUG_ENTER("tdc_remove_table");
  DBUG_PRINT("enter",("name: %s  remove_type: %d", table_name, remove_type));

  DBUG_ASSERT(remove_type == TDC_RT_REMOVE_UNUSED ||
              thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));

  if ((share= tdc_delete_share(db, table_name)))
  {
    I_P_List <TABLE, TABLE_share> purge_tables;
    uint my_refs= 1;

    mysql_mutex_lock(&share->tdc.LOCK_table_share);
    tc_wait_for_mdl_deadlock_detector(share);
    /*
      Mark share flushed in order to ensure that it gets
      automatically deleted once it is no longer referenced.

      Note that code in TABLE_SHARE::wait_for_old_version() assumes that
      marking share flushed is followed by purge of unused table
      shares.
    */
    if (remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE)
      share->tdc.flushed= true;

    while ((table= share->tdc.free_tables.pop_front()))
    {
      tc_remove_table(table);
      purge_tables.push_front(table);
    }
    if (kill_delayed_threads)
      kill_delayed_threads_for_table(share);

    if (remove_type == TDC_RT_REMOVE_NOT_OWN ||
        remove_type == TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE)
    {
      TABLE_SHARE::All_share_tables_list::Iterator it(share->tdc.all_tables);
      while ((table= it++))
      {
        my_refs++;
        DBUG_ASSERT(table->in_use == thd);
      }
    }
    DBUG_ASSERT(share->tdc.all_tables.is_empty() || remove_type != TDC_RT_REMOVE_ALL);
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);

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
      mysql_mutex_lock(&share->tdc.LOCK_table_share);
      while (share->tdc.ref_count > my_refs)
        mysql_cond_wait(&share->tdc.COND_release, &share->tdc.LOCK_table_share);
      mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    }

    tdc_release_share(share);

    found= true;
  }
  DBUG_ASSERT(found || remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE);
  DBUG_RETURN(found);
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
  TABLE_SHARE *share;
  int res= FALSE;

  if ((share= tdc_lock_share(db, table_name)))
  {
    if (share->tdc.flushed && refresh_version > share->tdc.version)
    {
      struct timespec abstime;
      set_timespec(abstime, wait_timeout);
      res= share->wait_for_old_version(thd, &abstime, deadlock_weight);
    }
    else
      tdc_unlock_share(share);
  }
  return res;
}


ulong tdc_refresh_version(void)
{
  my_atomic_rwlock_rdlock(&LOCK_tdc_atomics);
  ulong v= my_atomic_load64(&tdc_version);
  my_atomic_rwlock_rdunlock(&LOCK_tdc_atomics);
  return v;
}


ulong tdc_increment_refresh_version(void)
{
  my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
  ulong v= my_atomic_add64(&tdc_version, 1);
  my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  DBUG_PRINT("tcache", ("incremented global refresh_version to: %lu", v));
  return v + 1;
}


/**
  Initialize table definition cache iterator.
*/

void TDC_iterator::init(void)
{
  DBUG_ENTER("TDC_iterator::init");
  idx= 0;
  mysql_rwlock_rdlock(&LOCK_tdc);
  DBUG_VOID_RETURN;
}


/**
  Deinitialize table definition cache iterator.
*/

void TDC_iterator::deinit(void)
{
  DBUG_ENTER("TDC_iterator::deinit");
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_VOID_RETURN;
}


/**
  Get next TABLE_SHARE object from table definition cache.

  Object is protected against removal from table definition cache.

  @note Returned TABLE_SHARE is not guaranteed to be fully initialized:
  tdc_acquire_share() added new share, but didn't open it yet. If caller
  needs fully initializer share, it must lock table share mutex.
*/

TABLE_SHARE *TDC_iterator::next(void)
{
  TABLE_SHARE *share= 0;
  DBUG_ENTER("TDC_iterator::next");
  if (idx < tdc_hash.records)
  {
    share= (TABLE_SHARE*) my_hash_element(&tdc_hash, idx);
    idx++;
  }
  DBUG_RETURN(share);
}


/*
  Function to assign a new table map id to a table share.

  PARAMETERS

    share - Pointer to table share structure

  DESCRIPTION

    We are intentionally not checking that share->mutex is locked
    since this function should only be called when opening a table
    share and before it is entered into the table definition cache
    (meaning that it cannot be fetched by another thread, even
    accidentally).

  PRE-CONDITION(S)

    share is non-NULL
    last_table_id_lock initialized (tdc_inited)

  POST-CONDITION(S)

    share->table_map_id is given a value that with a high certainty is
    not used by any other table (the only case where a table id can be
    reused is on wrap-around, which means more than 4 billion table
    share opens have been executed while one table was open all the
    time).

    share->table_map_id is not ~0UL.
*/

void tdc_assign_new_table_id(TABLE_SHARE *share)
{
  ulong tid;
  DBUG_ENTER("assign_new_table_id");
  DBUG_ASSERT(share);
  DBUG_ASSERT(tdc_inited);

  /*
    There is one reserved number that cannot be used.  Remember to
    change this when 6-byte global table id's are introduced.
  */
  do
  {
    my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
    tid= my_atomic_add64(&last_table_id, 1);
    my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  } while (unlikely(tid == ~0UL));

  share->table_map_id= tid;
  DBUG_PRINT("info", ("table_id= %lu", share->table_map_id));
  DBUG_VOID_RETURN;
}
