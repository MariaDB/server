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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

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
  - close_cached_tables(): flush tables on shutdown
  - alloc_table_share()
  - free_table_share()

  Table cache invariants:
  - TABLE_SHARE::free_tables shall not contain objects with TABLE::in_use != 0
  - TABLE_SHARE::free_tables shall not receive new objects if
    TABLE_SHARE::tdc.flushed is true
*/

#include "mariadb.h"
#include "lf.h"
#include "table.h"
#include "sql_base.h"


/** Configuration. */
ulong tdc_size; /**< Table definition cache threshold for LRU eviction. */
ulong tc_size; /**< Table cache threshold for LRU eviction. */
uint32 tc_instances;
static std::atomic<uint32_t> tc_active_instances(1);
static std::atomic<bool> tc_contention_warning_reported;

/** Data collections. */
static LF_HASH tdc_hash; /**< Collection of TABLE_SHARE objects. */
/** Collection of unused TABLE_SHARE objects. */
static
I_P_List <TDC_element,
          I_P_List_adapter<TDC_element, &TDC_element::next, &TDC_element::prev>,
          I_P_List_null_counter,
          I_P_List_fast_push_back<TDC_element> > unused_shares;

static bool tdc_inited;


/**
  Protects unused shares list.

  TDC_element::prev
  TDC_element::next
  unused_shares
*/

static mysql_mutex_t LOCK_unused_shares;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_unused_shares, key_TABLE_SHARE_LOCK_table_share,
                     key_LOCK_table_cache;
static PSI_mutex_info all_tc_mutexes[]=
{
  { &key_LOCK_unused_shares, "LOCK_unused_shares", PSI_FLAG_GLOBAL },
  { &key_TABLE_SHARE_LOCK_table_share, "TABLE_SHARE::tdc.LOCK_table_share", 0 },
  { &key_LOCK_table_cache, "LOCK_table_cache", 0 }
};

static PSI_cond_key key_TABLE_SHARE_COND_release;
static PSI_cond_info all_tc_conds[]=
{
  { &key_TABLE_SHARE_COND_release, "TABLE_SHARE::tdc.COND_release", 0 }
};
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

struct Table_cache_instance
{
  /**
    Protects free_tables (TABLE::global_free_next and TABLE::global_free_prev),
    records, Share_free_tables::List (TABLE::prev and TABLE::next),
    TABLE::in_use.
  */
  mysql_mutex_t LOCK_table_cache;
  I_P_List <TABLE, I_P_List_adapter<TABLE, &TABLE::global_free_next,
                                    &TABLE::global_free_prev>,
            I_P_List_null_counter, I_P_List_fast_push_back<TABLE> >
    free_tables;
  ulong records;
  uint mutex_waits;
  uint mutex_nowaits;
  /** Avoid false sharing between instances */
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];

  Table_cache_instance(): records(0), mutex_waits(0), mutex_nowaits(0)
  {
    mysql_mutex_init(key_LOCK_table_cache, &LOCK_table_cache,
                     MY_MUTEX_INIT_FAST);
  }

  ~Table_cache_instance()
  {
    mysql_mutex_destroy(&LOCK_table_cache);
    DBUG_ASSERT(free_tables.is_empty());
    DBUG_ASSERT(records == 0);
  }

  /**
    Lock table cache mutex and check contention.

    Instance is considered contested if more than 20% of mutex acquisiotions
    can't be served immediately. Up to 100 000 probes may be performed to avoid
    instance activation on short sporadic peaks. 100 000 is estimated maximum
    number of queries one instance can serve in one second.

    These numbers work well on a 2 socket / 20 core / 40 threads Intel Broadwell
    system, that is expected number of instances is activated within reasonable
    warmup time. It may have to be adjusted for other systems.

    Only TABLE object acquistion is instrumented. We intentionally avoid this
    overhead on TABLE object release. All other table cache mutex acquistions
    are considered out of hot path and are not instrumented either.
  */
  void lock_and_check_contention(uint32_t n_instances, uint32_t instance)
  {
    if (mysql_mutex_trylock(&LOCK_table_cache))
    {
      mysql_mutex_lock(&LOCK_table_cache);
      if (++mutex_waits == 20000)
      {
        if (n_instances < tc_instances)
        {
          if (tc_active_instances.
              compare_exchange_weak(n_instances, n_instances + 1,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed))
          {
            sql_print_information("Detected table cache mutex contention at instance %d: "
                                  "%d%% waits. Additional table cache instance "
                                  "activated. Number of instances after "
                                  "activation: %d.",
                                  instance + 1,
                                  mutex_waits * 100 / (mutex_nowaits + mutex_waits),
                                  n_instances + 1);
          }
        }
        else if (!tc_contention_warning_reported.exchange(true,
                                                 std::memory_order_relaxed))
        {
          sql_print_warning("Detected table cache mutex contention at instance %d: "
                            "%d%% waits. Additional table cache instance "
                            "cannot be activated: consider raising "
                            "table_open_cache_instances. Number of active "
                            "instances: %d.",
                            instance + 1,
                            mutex_waits * 100 / (mutex_nowaits + mutex_waits),
                            n_instances);
        }
        mutex_waits= 0;
        mutex_nowaits= 0;
      }
    }
    else if (++mutex_nowaits == 80000)
    {
      mutex_waits= 0;
      mutex_nowaits= 0;
    }
  }
};


static Table_cache_instance *tc;


static void intern_close_table(TABLE *table)
{
  delete table->triggers;
  DBUG_ASSERT(table->file);
  closefrm(table);
  tdc_release_share(table->s);
  my_free(table);
}


/**
  Get number of TABLE objects (used and unused) in table cache.
*/

uint tc_records(void)
{
  ulong total= 0;
  for (uint32 i= 0; i < tc_instances; i++)
  {
    mysql_mutex_lock(&tc[i].LOCK_table_cache);
    total+= tc[i].records;
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
  }
  return total;
}


/**
  Remove TABLE object from table cache.
*/

static void tc_remove_table(TABLE *table)
{
  TDC_element *element= table->s->tdc;

  mysql_mutex_lock(&element->LOCK_table_share);
  /* Wait for MDL deadlock detector to complete traversing tdc.all_tables. */
  while (element->all_tables_refs)
    mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
  element->all_tables.remove(table);
  mysql_mutex_unlock(&element->LOCK_table_share);

  intern_close_table(table);
}


static void tc_remove_all_unused_tables(TDC_element *element,
                                        Share_free_tables::List *purge_tables)
{
  for (uint32 i= 0; i < tc_instances; i++)
  {
    mysql_mutex_lock(&tc[i].LOCK_table_cache);
    while (auto table= element->free_tables[i].list.pop_front())
    {
      tc[i].records--;
      tc[i].free_tables.remove(table);
      DBUG_ASSERT(element->all_tables_refs == 0);
      element->all_tables.remove(table);
      purge_tables->push_front(table);
    }
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
  }
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

static my_bool tc_purge_callback(TDC_element *element,
                                 Share_free_tables::List *purge_tables)
{
  mysql_mutex_lock(&element->LOCK_table_share);
  tc_remove_all_unused_tables(element, purge_tables);
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


void tc_purge()
{
  Share_free_tables::List purge_tables;

  tdc_iterate(0, (my_hash_walk_action) tc_purge_callback, &purge_tables);
  while (auto table= purge_tables.pop_front())
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
  uint32_t i=
    thd->thread_id % tc_active_instances.load(std::memory_order_relaxed);
  TABLE *LRU_table= 0;
  TDC_element *element= table->s->tdc;

  DBUG_ASSERT(table->in_use == thd);
  table->instance= i;
  mysql_mutex_lock(&element->LOCK_table_share);
  /* Wait for MDL deadlock detector to complete traversing tdc.all_tables. */
  while (element->all_tables_refs)
    mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
  element->all_tables.push_front(table);
  mysql_mutex_unlock(&element->LOCK_table_share);

  mysql_mutex_lock(&tc[i].LOCK_table_cache);
  if (tc[i].records == tc_size)
  {
    if ((LRU_table= tc[i].free_tables.pop_front()))
    {
      LRU_table->s->tdc->free_tables[i].list.remove(LRU_table);
      /* Needed if MDL deadlock detector chimes in before tc_remove_table() */
      LRU_table->in_use= thd;
      mysql_mutex_unlock(&tc[i].LOCK_table_cache);
      /* Keep out of locked LOCK_table_cache */
      tc_remove_table(LRU_table);
    }
    else
    {
      tc[i].records++;
      mysql_mutex_unlock(&tc[i].LOCK_table_cache);
    }
    /* Keep out of locked LOCK_table_cache */
    status_var_increment(thd->status_var.table_open_cache_overflows);
  }
  else
  {
    tc[i].records++;
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
  }
}


/**
  Acquire TABLE object from table cache.

  @pre share must be protected against removal.

  Acquired object cannot be evicted or acquired again.

  @return TABLE object, or NULL if no unused objects.
*/

TABLE *tc_acquire_table(THD *thd, TDC_element *element)
{
  uint32_t n_instances= tc_active_instances.load(std::memory_order_relaxed);
  uint32_t i= thd->thread_id % n_instances;
  TABLE *table;

  tc[i].lock_and_check_contention(n_instances, i);
  table= element->free_tables[i].list.pop_front();
  if (table)
  {
    DBUG_ASSERT(!table->in_use);
    table->in_use= thd;
    /* The ex-unused table must be fully functional. */
    DBUG_ASSERT(table->db_stat && table->file);
    /* The children must be detached from the table. */
    DBUG_ASSERT(!table->file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
    tc[i].free_tables.remove(table);
  }
  mysql_mutex_unlock(&tc[i].LOCK_table_cache);
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

void tc_release_table(TABLE *table)
{
  uint32 i= table->instance;
  DBUG_ENTER("tc_release_table");
  DBUG_ASSERT(table->in_use);
  DBUG_ASSERT(table->file);
  DBUG_ASSERT(!table->pos_in_locked_tables);

  mysql_mutex_lock(&tc[i].LOCK_table_cache);
  if (table->needs_reopen() || table->s->tdc->flushed ||
      tc[i].records > tc_size)
  {
    tc[i].records--;
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
    tc_remove_table(table);
  }
  else
  {
    table->in_use= 0;
    table->s->tdc->free_tables[i].list.push_front(table);
    tc[i].free_tables.push_back(table);
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
  }
  DBUG_VOID_RETURN;
}


static void tdc_assert_clean_share(TDC_element *element)
{
  DBUG_ASSERT(element->share == 0);
  DBUG_ASSERT(element->ref_count == 0);
  DBUG_ASSERT(element->m_flush_tickets.is_empty());
  DBUG_ASSERT(element->all_tables.is_empty());
#ifndef DBUG_OFF
  for (uint32 i= 0; i < tc_instances; i++)
    DBUG_ASSERT(element->free_tables[i].list.is_empty());
#endif
  DBUG_ASSERT(element->all_tables_refs == 0);
  DBUG_ASSERT(element->next == 0);
  DBUG_ASSERT(element->prev == 0);
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
  tdc_assert_clean_share(element);
  lf_hash_delete(&tdc_hash, pins, element->m_key, element->m_key_length);
  if (!thd)
    lf_hash_put_pins(pins);
  free_table_share(share);
  DBUG_VOID_RETURN;
}


/**
  Prepeare table share for use with table definition cache.
*/

static void lf_alloc_constructor(uchar *arg)
{
  TDC_element *element= (TDC_element*) (arg + LF_HASH_OVERHEAD);
  DBUG_ENTER("lf_alloc_constructor");
  mysql_mutex_init(key_TABLE_SHARE_LOCK_table_share,
                   &element->LOCK_table_share, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_TABLE_SHARE_COND_release, &element->COND_release, 0);
  element->m_flush_tickets.empty();
  element->all_tables.empty();
  for (uint32 i= 0; i < tc_instances; i++)
    element->free_tables[i].list.empty();
  element->all_tables_refs= 0;
  element->share= 0;
  element->ref_count= 0;
  element->next= 0;
  element->prev= 0;
  DBUG_VOID_RETURN;
}


/**
  Release table definition cache specific resources of table share.
*/

static void lf_alloc_destructor(uchar *arg)
{
  TDC_element *element= (TDC_element*) (arg + LF_HASH_OVERHEAD);
  DBUG_ENTER("lf_alloc_destructor");
  tdc_assert_clean_share(element);
  mysql_cond_destroy(&element->COND_release);
  mysql_mutex_destroy(&element->LOCK_table_share);
  DBUG_VOID_RETURN;
}


static void tdc_hash_initializer(LF_HASH *,
                                 TDC_element *element, LEX_STRING *key)
{
  memcpy(element->m_key, key->str, key->length);
  element->m_key_length= (uint)key->length;
  tdc_assert_clean_share(element);
}


static uchar *tdc_hash_key(const TDC_element *element, size_t *length,
                           my_bool)
{
  *length= element->m_key_length;
  return (uchar*) element->m_key;
}


/**
  Initialize table definition cache.
*/

bool tdc_init(void)
{
  DBUG_ENTER("tdc_init");
#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_register("sql", all_tc_mutexes, array_elements(all_tc_mutexes));
  mysql_cond_register("sql", all_tc_conds, array_elements(all_tc_conds));
#endif
  /* Extra instance is allocated to avoid false sharing */
  if (!(tc= new Table_cache_instance[tc_instances + 1]))
    DBUG_RETURN(true);
  tdc_inited= true;
  mysql_mutex_init(key_LOCK_unused_shares, &LOCK_unused_shares,
                   MY_MUTEX_INIT_FAST);
  lf_hash_init(&tdc_hash, sizeof(TDC_element) +
                          sizeof(Share_free_tables) * (tc_instances - 1),
               LF_HASH_UNIQUE, 0, 0,
               (my_hash_get_key) tdc_hash_key,
               &my_charset_bin);
  tdc_hash.alloc.constructor= lf_alloc_constructor;
  tdc_hash.alloc.destructor= lf_alloc_destructor;
  tdc_hash.initializer= (lf_hash_initializer) tdc_hash_initializer;
  DBUG_RETURN(false);
}


/**
  Notify table definition cache that process of shutting down server
  has started so it has to keep number of TABLE and TABLE_SHARE objects
  minimal in order to reduce number of references to pluggable engines.
*/

void tdc_start_shutdown(void)
{
  DBUG_ENTER("tdc_start_shutdown");
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
    purge_tables();
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
    delete [] tc;
  }
  DBUG_VOID_RETURN;
}


/**
  Get number of cached table definitions.

  @return Number of cached table definitions
*/

ulong tdc_records(void)
{
  return lf_hash_size(&tdc_hash);
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
  if (unlikely(fix_thd_pins(thd)))
    DBUG_RETURN((TDC_element*) MY_ERRPTR);

  element= (TDC_element *) lf_hash_search(&tdc_hash, thd->tdc_hash_pins,
                                          (uchar*) key,
                                          tdc_create_key(key, db, table_name));
  if (element)
  {
    mysql_mutex_lock(&element->LOCK_table_share);
    if (unlikely(!element->share || element->share->error))
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


int tdc_share_is_cached(THD *thd, const char *db, const char *table_name)
{
  char key[MAX_DBKEY_LENGTH];

  if (unlikely(fix_thd_pins(thd)))
    return -1;

  if (lf_hash_search(&tdc_hash, thd->tdc_hash_pins, (uchar*) key,
                     tdc_create_key(key, db, table_name)))
  {
    lf_hash_search_unpin(thd->tdc_hash_pins);
    return 1;
  }
  return 0;
}


/*
  Get TABLE_SHARE for a table.

  tdc_acquire_share()
  thd                   Thread handle
  tl                    Table that should be opened
  flags                 operation: what to open table or view
  out_table             TABLE for the requested table

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

    if (!(share= alloc_table_share(tl->db.str, tl->table_name.str, key, key_length)))
    {
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    /* note that tdc_acquire_share() *always* uses discovery */
    open_table_def(thd, share, flags | GTS_USE_DISCOVERY);

    if (checked_unlikely(share->error))
    {
      free_table_share(share);
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    mysql_mutex_lock(&element->LOCK_table_share);
    element->share= share;
    share->tdc= element;
    element->ref_count++;
    element->flushed= false;
    mysql_mutex_unlock(&element->LOCK_table_share);

    tdc_purge(false);
    if (out_table)
    {
      status_var_increment(thd->status_var.table_open_cache_misses);
      *out_table= 0;
    }
    share->m_psi= PSI_CALL_get_table_share(false, share);
    goto end;
  }

  /* cannot force discovery of a cached share */
  DBUG_ASSERT(!(flags & GTS_FORCE_DISCOVERY));

  if (out_table && (flags & GTS_TABLE))
  {
    if ((*out_table= tc_acquire_table(thd, element)))
    {
      lf_hash_search_unpin(thd->tdc_hash_pins);
      DBUG_ASSERT(!(flags & GTS_NOLOCK));
      DBUG_ASSERT(element->share);
      DBUG_ASSERT(!element->share->error);
      DBUG_ASSERT(!element->share->is_view);
      status_var_increment(thd->status_var.table_open_cache_hits);
      DBUG_RETURN(element->share);
    }
    status_var_increment(thd->status_var.table_open_cache_misses);
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
  if (unlikely(share->error))
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
  DBUG_PRINT("exit", ("share: %p  ref_count: %u",
                      share, share->tdc->ref_count));
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
    share= UNUSABLE_TABLE_SHARE;
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
             ("share: %p  table: %s.%s  ref_count: %u",
              share, share->db.str, share->table_name.str,
              share->tdc->ref_count));
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
    if (!share->is_view)
      mysql_cond_broadcast(&share->tdc->COND_release);
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


void tdc_remove_referenced_share(THD *thd, TABLE_SHARE *share)
{
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, share->db.str,
                                             share->table_name.str,
                                             MDL_EXCLUSIVE));
  share->tdc->flush_unused(true);
  mysql_mutex_lock(&share->tdc->LOCK_table_share);
  DEBUG_SYNC(thd, "before_wait_for_refs");
  share->tdc->wait_for_refs(1);
  DBUG_ASSERT(share->tdc->all_tables.is_empty());
  share->tdc->ref_count--;
  tdc_delete_share_from_hash(share->tdc);
}


/**
   Removes all TABLE instances and corresponding TABLE_SHARE

   @param  thd          Thread context
   @param  db           Name of database
   @param  table_name   Name of table

   @note It assumes that table instances are already not used by any
   (other) thread (this should be achieved by using meta-data locks).
*/

void tdc_remove_table(THD *thd, const char *db, const char *table_name)
{
  TDC_element *element;
  DBUG_ENTER("tdc_remove_table");
  DBUG_PRINT("enter", ("name: %s", table_name));

  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));

  mysql_mutex_lock(&LOCK_unused_shares);
  if (!(element= tdc_lock_share(thd, db, table_name)))
  {
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
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
    DBUG_VOID_RETURN;
  }
  mysql_mutex_unlock(&LOCK_unused_shares);

  element->ref_count++;
  mysql_mutex_unlock(&element->LOCK_table_share);

  /* We have to relock the mutex to avoid code duplication. Sigh. */
  tdc_remove_referenced_share(thd, element->share);
  DBUG_VOID_RETURN;
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
                             ulong wait_timeout, uint deadlock_weight)
{
  TDC_element *element;

  if (!(element= tdc_lock_share(thd, db, table_name)))
    return FALSE;
  else if (element == MY_ERRPTR)
    return TRUE;
  else if (element->flushed)
  {
    struct timespec abstime;
    set_timespec(abstime, wait_timeout);
    return element->share->wait_for_old_version(thd, &abstime, deadlock_weight);
  }
  tdc_unlock_share(element);
  return FALSE;
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
    init_alloc_root(PSI_INSTRUMENT_ME, &no_dups_argument.root, 4096, 4096, MYF(alloc_flags));
    my_hash_init(PSI_INSTRUMENT_ME, &no_dups_argument.hash, &my_charset_bin,
                 tdc_records(), 0, 0, eliminate_duplicates_get_key, 0,
                 hash_flags);
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


int show_tc_active_instances(THD *thd, SHOW_VAR *var, char *buff,
                             enum enum_var_type scope)
{
  var->type= SHOW_UINT;
  var->value= buff;
  *(reinterpret_cast<uint32_t*>(buff))=
    tc_active_instances.load(std::memory_order_relaxed);
  return 0;
}


/**
  Waits until ref_count goes down to given number

  @param  my_refs  Number of references owned by the caller

  Caller must own at least one TABLE_SHARE reference.

  Even though current thread holds exclusive metadata lock on this share,
  concurrent FLUSH TABLES threads may be in process of closing unused table
  instances belonging to this share. E.g.:
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

void TDC_element::wait_for_refs(uint my_refs)
{
  while (ref_count > my_refs)
    mysql_cond_wait(&COND_release, &LOCK_table_share);
}


/**
  Flushes unused TABLE instances

  @param  thd          Thread context
  @param  mark_flushed Whether to destroy TABLE_SHARE when released

  Caller is allowed to own used TABLE instances.
  There must be no TABLE objects used by other threads and caller must own
  exclusive metadata lock on the table.
*/

void TDC_element::flush(THD *thd, bool mark_flushed)
{
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, share->db.str,
                                             share->table_name.str,
                                             MDL_EXCLUSIVE));

  flush_unused(mark_flushed);

  mysql_mutex_lock(&LOCK_table_share);
  All_share_tables_list::Iterator it(all_tables);
  uint my_refs= 0;
  while (auto table= it++)
  {
    if (table->in_use == thd)
      my_refs++;
  }
  wait_for_refs(my_refs);
#ifndef DBUG_OFF
  it.rewind();
  while (auto table= it++)
    DBUG_ASSERT(table->in_use == thd);
#endif
  mysql_mutex_unlock(&LOCK_table_share);
}


/**
  Flushes unused TABLE instances
*/

void TDC_element::flush_unused(bool mark_flushed)
{
  Share_free_tables::List purge_tables;

  mysql_mutex_lock(&LOCK_table_share);
  if (mark_flushed)
    flushed= true;
  tc_remove_all_unused_tables(this, &purge_tables);
  mysql_mutex_unlock(&LOCK_table_share);

  while (auto table= purge_tables.pop_front())
    intern_close_table(table);
}
