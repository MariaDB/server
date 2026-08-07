/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB Corporation.
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
#include "aligned.h"


/** Configuration. */
ulong tdc_size; /**< Table definition cache threshold for LRU eviction. */
ulong tc_size; /**< Table cache threshold for LRU eviction. */
uint32 tc_instances;
static size_t tc_allocated_size;
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
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  mysql_mutex_t LOCK_table_cache;
  I_P_List <TABLE, I_P_List_adapter<TABLE, &TABLE::global_free_next,
                                    &TABLE::global_free_prev>,
            I_P_List_null_counter, I_P_List_fast_push_back<TABLE> >
    free_tables;
  ulong records;
  uint mutex_waits;
  uint mutex_nowaits;

  Table_cache_instance(): records(0), mutex_waits(0), mutex_nowaits(0)
  {
    static_assert(!(sizeof(*this) % CPU_LEVEL1_DCACHE_LINESIZE), "alignment");
    mysql_mutex_init(key_LOCK_table_cache, &LOCK_table_cache,
                     MY_MUTEX_INIT_FAST);
  }

  ~Table_cache_instance()
  {
    mysql_mutex_destroy(&LOCK_table_cache);
    DBUG_ASSERT(free_tables.is_empty());
    DBUG_ASSERT(records == 0);
  }

  static void *operator new[](size_t size)
  { return aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE); }
  static void operator delete[](void *ptr) { aligned_free(ptr); }
  static void mark_memory_freed()
  {
    update_malloc_size(-(longlong) tc_allocated_size, 0);
  }

  /**
    Lock table cache mutex and check contention.

    Instance is considered contested if more than 20% of mutex acquisitions
    can't be served immediately. Up to 100 000 probes may be performed to avoid
    instance activation on short sporadic peaks. 100 000 is estimated maximum
    number of queries one instance can serve in one second.

    These numbers work well on a 2 socket / 20 core / 40 threads Intel Broadwell
    system, that is expected number of instances is activated within reasonable
    warmup time. It may have to be adjusted for other systems.

    Only TABLE object acquisition is instrumented. We intentionally avoid this
    overhead on TABLE object release. All other table cache mutex acquisitions
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
  TABLE_SHARE_VERSION *version= table->s->version;

  mysql_mutex_lock(&element->LOCK_table_share);
  /* Wait for MDL deadlock detector to complete traversing all_tables. */
  while (version->all_tables_refs)
    mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
  version->all_tables.remove(table);
  mysql_mutex_unlock(&element->LOCK_table_share);

  intern_close_table(table);
}


static void tc_remove_all_unused_tables(TABLE_SHARE_VERSION *version,
                                        Share_free_tables::List *purge_tables)
{
  for (uint32 i= 0; i < tc_instances; i++)
  {
    mysql_mutex_lock(&tc[i].LOCK_table_cache);
    while (auto table= version->free_tables[i].list.pop_front())
    {
      tc[i].records--;
      tc[i].free_tables.remove(table);
      DBUG_ASSERT(version->all_tables_refs == 0);
      version->all_tables.remove(table);
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
        periodically flush all not used tables.
*/

static my_bool tc_purge_callback(void *_element, void *_purge_tables)
{
  TDC_element *element= static_cast<TDC_element *>(_element);
  Share_free_tables::List *purge_tables=
      static_cast<Share_free_tables::List *>(_purge_tables);
  mysql_mutex_lock(&element->LOCK_table_share);
  /*
    Walk every version's free_tables. After a lock-free DDL the chain may
    briefly hold multiple versions (one CURRENT plus OLDER versions still
    pinned by in-flight transactions). OLDER versions have their
    free_tables drained at install time by tdc_install_version, so they
    are usually empty here; CURRENT may have idle cached TABLEs to purge.
  */
  for (TABLE_SHARE_VERSION *v= element->versions_head; v; v= v->next)
    tc_remove_all_unused_tables(v, purge_tables);
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


void tc_purge()
{
  Share_free_tables::List purge_tables;

  tdc_iterate(0, tc_purge_callback, &purge_tables);
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
  TABLE_SHARE_VERSION *version= table->s->version;

  DBUG_ASSERT(table->in_use == thd);
  table->instance= i;
  mysql_mutex_lock(&element->LOCK_table_share);
  /* Wait for MDL deadlock detector to complete traversing all_tables. */
  while (version->all_tables_refs)
    mysql_cond_wait(&element->COND_release, &element->LOCK_table_share);
  version->all_tables.push_front(table);
  mysql_mutex_unlock(&element->LOCK_table_share);

  mysql_mutex_lock(&tc[i].LOCK_table_cache);
  if (tc[i].records == tc_size)
  {
    if ((LRU_table= tc[i].free_tables.pop_front()))
    {
      LRU_table->s->version->free_tables[i].list.remove(LRU_table);
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

TABLE *tc_acquire_table(THD *thd, TABLE_SHARE_VERSION *version)
{
  DBUG_ASSERT(version);
  uint32_t n_instances= tc_active_instances.load(std::memory_order_relaxed);
  uint32_t i= thd->thread_id % n_instances;
  TABLE *table;

  tc[i].lock_and_check_contention(n_instances, i);
  table= version->free_tables[i].list.pop_front();
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
  if (table->needs_reopen() || table->s->version->flushed ||
      tc[i].records > tc_size)
  {
    tc[i].records--;
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
    tc_remove_table(table);
  }
  else
  {
    table->in_use= 0;
    table->s->version->free_tables[i].list.push_front(table);
    tc[i].free_tables.push_back(table);
    mysql_mutex_unlock(&tc[i].LOCK_table_cache);
  }
  DBUG_VOID_RETURN;
}


/*
  Assert that the element is "clean": no version chain, no flush tickets,
  not on the unused_shares LRU. Per-version state (all_tables, free_tables,
  ref_count, etc.) is asserted by tdc_free_version on each version freed.
  Called before the LF_HASH slot is reused or destroyed.
*/

static void tdc_assert_clean_share(TDC_element *element)
{
  DBUG_ASSERT(element->versions_head == 0);
  DBUG_ASSERT(element->versions_tail == 0);
  DBUG_ASSERT(element->m_flush_tickets.is_empty());
  DBUG_ASSERT(element->next == 0);
  DBUG_ASSERT(element->prev == 0);
}


/**
  Delete share from hash and free share object.

  Precondition: the version chain holds exactly one version (the CURRENT
  one we're about to free). Callers must ensure all OLDER versions have
  been GC'd before reaching here. The DBUG_ASSERTs below enforce this.

  Reasons this holds for current callers:
    - tdc_release_share's CURRENT path checks versions_head == versions_tail
      before invoking this (or pushing to unused_shares).
    - tdc_remove_referenced_share is called under X MDL after a drain; any
      OLDER versions would have been GC'd when conflicting transactions
      released their bindings.
    - tdc_remove_table goes through the same X-MDL flow.
    - tdc_purge picks elements off unused_shares, which were placed there
      by tdc_release_share under the same chain-length check.
*/

static void tdc_delete_share_from_hash(TDC_element *element)
{
  THD *thd= current_thd;
  LF_PINS *pins;
  TABLE_SHARE *share;
  TABLE_SHARE_VERSION *version;
  DBUG_ENTER("tdc_delete_share_from_hash");

  mysql_mutex_assert_owner(&element->LOCK_table_share);
  version= element->current();
  DBUG_ASSERT(version);
  /* Precondition: chain length 1. */
  DBUG_ASSERT(version == element->versions_head);
  DBUG_ASSERT(version->next == 0);
  DBUG_ASSERT(version->prev == 0);
  share= version->share;
  DBUG_ASSERT(share);
  version->share= 0;
  element->versions_head= 0;
  element->versions_tail= 0;
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
  tdc_free_version(version);
  DBUG_VOID_RETURN;
}


/**
  Prepare table share for use with table definition cache.
*/

static void lf_alloc_constructor(uchar *arg)
{
  TDC_element *element= (TDC_element*) (arg + LF_HASH_OVERHEAD);
  DBUG_ENTER("lf_alloc_constructor");
  mysql_mutex_init(key_TABLE_SHARE_LOCK_table_share,
                   &element->LOCK_table_share, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_TABLE_SHARE_COND_release, &element->COND_release, 0);
  element->m_flush_tickets.empty();
  element->versions_head= 0;
  element->versions_tail= 0;
  element->next_schema_version= 0;
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


/**
  Allocate and initialize a TABLE_SHARE_VERSION with a trailing free_tables[]
  array sized to tc_instances. Caller is responsible for installing the
  version into a TDC_element via tdc_install_version() under LOCK_table_share.
*/

TABLE_SHARE_VERSION *tdc_alloc_version()
{
  size_t size= sizeof(TABLE_SHARE_VERSION) +
               sizeof(Share_free_tables) * (tc_instances - 1);
  TABLE_SHARE_VERSION *v= (TABLE_SHARE_VERSION*)
    my_malloc(PSI_INSTRUMENT_ME, size, MYF(MY_WME | MY_ZEROFILL));
  if (!v)
    return NULL;
  v->all_tables.empty();
  for (uint32 i= 0; i < tc_instances; i++)
    v->free_tables[i].list.empty();
  return v;
}


/**
  Append a TABLE_SHARE_VERSION to a TDC_element's chain as the new tail
  (i.e. the new "current" version served to new opens). Assigns
  schema_version from the element's per-element monotonic counter.

  If the chain already had a tail, that tail is demoted to OLDER: marked
  flushed (so future tc_release_table calls destroy its TABLEs rather
  than cache them) and its existing cached free_tables[] are drained
  eagerly. In-use TABLEs of the demoted version are untouched and continue
  serving their statements; they get destroyed on their next release.

  Takes e->LOCK_table_share internally; caller should not hold it.
*/

void tdc_install_version(TDC_element *e, TABLE_SHARE_VERSION *v)
{
  DBUG_ASSERT(v->next == 0 && v->prev == 0);
  DBUG_ASSERT(v->share);

  /*
    Cached TABLEs in the previous tail's free_tables[] become unreachable
    once we demote that tail to OLDER (no new transaction will bind to an
    OLDER version, and existing v1-bound transactions will allocate fresh
    TABLEs on their next open if free_tables is empty). We mark the
    previous tail flushed so future tc_release_table calls destroy v1's
    TABLEs instead of caching them, and we eagerly drain whatever idle
    TABLEs are already in free_tables[] here. In-use TABLEs (table->in_use
    != 0) are not in free_tables[] by invariant; they continue serving
    their statement and get destroyed later when that statement ends.
    Once both routes drop v1's ref_count to 0, tdc_release_share's OLDER
    branch GCs the version.
  */
  Share_free_tables::List purge_tables;

  mysql_mutex_lock(&e->LOCK_table_share);
  v->schema_version= ++e->next_schema_version;
  v->prev= e->versions_tail;
  v->next= 0;
  if (e->versions_tail)
  {
    e->versions_tail->next= v;
    e->versions_tail->flushed= true;
    tc_remove_all_unused_tables(e->versions_tail, &purge_tables);
  }
  e->versions_tail= v;
  if (!e->versions_head)
    e->versions_head= v;
  v->share->version= v;
  mysql_mutex_unlock(&e->LOCK_table_share);

  while (auto table= purge_tables.pop_front())
    intern_close_table(table);
}


/**
  Remove an OLDER (i.e. non-tail) TABLE_SHARE_VERSION from a TDC_element's
  chain and free its TABLE_SHARE and the version itself.

  Called from tdc_release_share's OLDER branch when an OLDER version's
  ref_count drops to 0 (the last in-use TABLE was destroyed or the last
  binding to it was released).

  Caller must own e->LOCK_table_share. v must satisfy:
    - v != e->versions_tail (the current version is never GC'd here;
      it's freed by tdc_delete_share_from_hash when the element is removed)
    - v->ref_count == 0
*/

void tdc_gc_version(TDC_element *e, TABLE_SHARE_VERSION *v)
{
  mysql_mutex_assert_owner(&e->LOCK_table_share);
  DBUG_ASSERT(v != e->versions_tail);
  DBUG_ASSERT(v->ref_count == 0);

  if (v->prev)
    v->prev->next= v->next;
  if (v->next)
    v->next->prev= v->prev;
  if (v == e->versions_head)
    e->versions_head= v->next;

  TABLE_SHARE *share= v->share;
  v->share= 0;
  v->next= v->prev= 0;
  free_table_share(share);
  tdc_free_version(v);
}


/**
  Free a TABLE_SHARE_VERSION. The caller must have already detached it from
  any TDC_element's chain and freed the underlying TABLE_SHARE (so
  v->share is 0). DBUG_ASSERTs below enforce the expected clean state:
  no share, no refs, empty all_tables, no MDL deadlock-detector refs on
  all_tables, and empty per-instance free_tables[].
*/

void tdc_free_version(TABLE_SHARE_VERSION *v)
{
  DBUG_ASSERT(v->share == 0);
  DBUG_ASSERT(v->ref_count == 0);
  DBUG_ASSERT(v->all_tables.is_empty());
  DBUG_ASSERT(v->all_tables_refs == 0);
#ifndef DBUG_OFF
  for (uint32 i= 0; i < tc_instances; i++)
    DBUG_ASSERT(v->free_tables[i].list.is_empty());
#endif
  my_free(v);
}


static void tdc_hash_initializer(LF_HASH *,
                                 void *_element, const void *_key)
{
  TDC_element *element= static_cast<TDC_element *>(_element);
  const LEX_STRING *key= static_cast<const LEX_STRING *>(_key);
  memcpy(element->m_key, key->str, key->length);
  element->m_key_length= (uint)key->length;
  tdc_assert_clean_share(element);
}


static const uchar *tdc_hash_key(const void *element_, size_t *length, my_bool)
{
  auto element= static_cast<const TDC_element *>(element_);
  *length= element->m_key_length;
  return reinterpret_cast<const uchar *>(element->m_key);
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
  tc_allocated_size= (tc_instances + 1) * sizeof *tc;
  update_malloc_size(tc_allocated_size, 0);
  tdc_inited= true;
  mysql_mutex_init(key_LOCK_unused_shares, &LOCK_unused_shares,
                   MY_MUTEX_INIT_FAST);
  lf_hash_init(&tdc_hash, sizeof(TDC_element),
               LF_HASH_UNIQUE, 0, 0, tdc_hash_key, &my_charset_bin);
  tdc_hash.alloc.constructor= lf_alloc_constructor;
  tdc_hash.alloc.destructor= lf_alloc_destructor;
  tdc_hash.initializer= tdc_hash_initializer;
  DBUG_RETURN(false);
}


/**
  Notify table definition cache that process of shutting down server
  has started so it has to keep number of TABLE and TABLE_SHARE objects
  minimal in order to reduce number of references to pluggable engines.
*/

void tdc_start_shutdown(bool use_dummy_thd)
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

    THD *thd= nullptr;
    bool reset_back_current_thd= false;

    if (use_dummy_thd && _current_thd() == nullptr)
    {
      /*
        There is no active THD, use the surrogate one to handle memory
        allocations in Sql_alloc::operator new() that uses current_thd()
        for getting THD to get access to memory allocation API
      */
      thd= new THD(0);
      DBUG_ASSERT(thd != nullptr);

      thd->store_globals();
      thd->init();
      thd->set_query_inner((char*) STRING_WITH_LEN("intern:tdc_start_shutdown"),
                           default_charset_info);

      reset_back_current_thd= true;
    }
    /* Free all cached but unused TABLEs and TABLE_SHAREs. */
    /*
      purge_tables() iterates along opened tables close every one of them.
      In case tables use triggers it results in destroying sp_head objects
      for every associated trigger. Destroying an object of the sp_head class
      leads to deleting sp_instr objects. In case some of SP instruction
      were previously re-parsed by the reason of changes in tables metadata,
      the method
         sp_head::register_instr_mem_root_for_deallocation()
      is called to add a memory root used for re-parsing to the list of
      mem_roots to be freed on sp_head destruction. Mem_roots for later
      destruction are placed on the List that use current_thd() for memory
      allocation. That is the reason why the dummy THD is created before
      in case there is no active THD at the moment the function
      tdc_start_shutdown is invoked.
    */
    purge_tables();

    if (reset_back_current_thd)
    {
      thd->reset_globals();
      delete thd;
    }
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
    if (tc)
    {
      tc->mark_memory_freed();
      delete [] tc;
      tc= 0;
    }
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
    if (element->current() && element->current()->ref_count)
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
    if (unlikely(!element->current() ||
                 !element->current()->share || element->current()->share->error))
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
  TABLE_SHARE_VERSION *bound= NULL;
  TABLE_SHARE_VERSION *cur;
  const char *key;
  uint key_length= get_table_def_key(tl, &key);
  my_hash_value_type hash_value= tl->mdl_request.key.tc_hash_value();
  bool was_unused;
  DBUG_ENTER("tdc_acquire_share");

  if (fix_thd_pins(thd))
    DBUG_RETURN(0);

  /*
    If this transaction is already bound to a version of this name, use
    that version (which may be OLDER than versions_tail when DDL has
    installed newer versions). The binding pins the version via
    ref_count, so it stays alive for the rest of this transaction.

    GTS_FORCE_DISCOVERY and GTS_NOLOCK callers don't want a binding-based
    fast path — they explicitly want to re-read from disk or merely probe
    existence — so they go through the lf_hash lookup path.
  */
  if (!(flags & (GTS_FORCE_DISCOVERY | GTS_NOLOCK)))
    bound= thd->lookup_schema_binding((const uchar*) key, key_length);

  if (bound)
  {
    element= bound->share->tdc;
    share= bound->share;
    DBUG_ASSERT(share->tdc == element);

    if (out_table && (flags & GTS_TABLE))
    {
      if ((*out_table= tc_acquire_table(thd, bound)))
      {
        DBUG_ASSERT(!share->error);
        DBUG_ASSERT(!share->is_view);
        status_var_increment(thd->status_var.table_open_cache_hits);
        goto end;
      }
      status_var_increment(thd->status_var.table_open_cache_misses);
    }

    mysql_mutex_lock(&element->LOCK_table_share);
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
    /*
      Bound version has ref_count >= 1 (the binding pin), so was_unused is
      always false here — no unused_shares LRU dance needed.
    */
    bound->ref_count++;
    mysql_mutex_unlock(&element->LOCK_table_share);
    goto end;
  }

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
    /* It's safe to unpin the pins here, because an empty element was inserted
    above, "empty" means at least element->versions_tail = 0. Some other
    thread can't delete it while versions_tail == 0. And the chain is
    protected with element->LOCK_table_share mutex. */
    lf_hash_search_unpin(thd->tdc_hash_pins);
    DBUG_ASSERT(element);

    TABLE_SHARE_VERSION *version= tdc_alloc_version();
    if (!version)
    {
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    if (!(share= alloc_table_share(tl->db.str, tl->table_name.str, key, key_length)))
    {
      tdc_free_version(version);
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    /* note that tdc_acquire_share() *always* uses discovery */
    open_table_def(thd, share, flags | GTS_USE_DISCOVERY);

    if (checked_unlikely(share->error))
    {
      free_table_share(share);
      tdc_free_version(version);
      lf_hash_delete(&tdc_hash, thd->tdc_hash_pins, key, key_length);
      DBUG_RETURN(0);
    }

    /*
      version is freshly allocated and not yet visible to other threads;
      the field sets below don't need LOCK_table_share. tdc_install_version
      takes the lock internally to chain the version into element->versions_*.
    */
    version->share= share;
    version->ref_count= 1;
    version->flushed= false;
    share->tdc= element;
    tdc_install_version(element, version);

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

  /*
    The LF_HASH pin protects the TDC_element memory but NOT the heap-allocated
    TABLE_SHARE_VERSION it points to: a concurrent tdc_purge can drop the
    element off unused_shares LRU and free its versions while we still hold
    the pin. Take LOCK_table_share before reading element->current() so the
    version stays pinned while we use it. The lock is held continuously
    through both the cache-hit fast path and the slow path; tc_acquire_table
    nests fine because LOCK_table_share → tc[i].LOCK_table_cache is the same
    ordering tc_remove_all_unused_tables already uses.
  */
  mysql_mutex_lock(&element->LOCK_table_share);
  cur= element->current();
  if (!cur)
  {
    mysql_mutex_unlock(&element->LOCK_table_share);
    lf_hash_search_unpin(thd->tdc_hash_pins);
    std::this_thread::yield();
    goto retry;
  }

  if (out_table && (flags & GTS_TABLE))
  {
    if ((*out_table= tc_acquire_table(thd, cur)))
    {
      DBUG_ASSERT(!(flags & GTS_NOLOCK));
      DBUG_ASSERT(cur->share);
      DBUG_ASSERT(!cur->share->error);
      DBUG_ASSERT(!cur->share->is_view);
      status_var_increment(thd->status_var.table_open_cache_hits);
      share= cur->share;
      mysql_mutex_unlock(&element->LOCK_table_share);
      lf_hash_search_unpin(thd->tdc_hash_pins);
      goto end;
    }
    status_var_increment(thd->status_var.table_open_cache_misses);
  }

  if (!(share= cur->share))
  {
    mysql_mutex_unlock(&element->LOCK_table_share);
    lf_hash_search_unpin(thd->tdc_hash_pins);
    std::this_thread::yield();
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

  was_unused= !cur->ref_count;
  cur->ref_count++;
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
                      share, share->version->ref_count));
  /*
    Record that this transaction is using the current version of this
    name. The binding pins the version via ref_count and is released at
    end of transaction by MDL_context::release_transactional_locks().

    If we took the bound branch above, `bound` is non-NULL and we already
    have a binding to that version — skip. Otherwise install a binding to
    the version we just acquired (which is current() since the unbound
    path always uses current()). Skip for GTS_NOLOCK callers (they're
    just probing existence).
  */
  if (!(flags & GTS_NOLOCK) && !bound)
  {
    (void) thd->add_schema_binding((const uchar*) key, key_length,
                                   share->version);
  }
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
  TABLE_SHARE_VERSION *v= share->version;
  TDC_element *e= share->tdc;
  DBUG_ENTER("tdc_release_share");
  DBUG_ASSERT(v);

  mysql_mutex_lock(&e->LOCK_table_share);
  DBUG_PRINT("enter",
             ("share: %p  table: %s.%s  ref_count: %u",
              share, share->db.str, share->table_name.str, v->ref_count));
  DBUG_ASSERT(v->ref_count);

  /*
    OLDER version: not visible to new opens. Decrement and, if no refs
    remain, GC the version. No LRU/eviction logic — OLDER versions are
    transient; they only exist while in-flight transactions still pin them.
  */
  if (v != e->versions_tail)
  {
    --v->ref_count;
    if (!share->is_view)
      mysql_cond_broadcast(&e->COND_release);
    if (v->ref_count == 0)
      tdc_gc_version(e, v);
    mysql_mutex_unlock(&e->LOCK_table_share);
    DBUG_VOID_RETURN;
  }

  /* CURRENT (versions_tail) — existing LRU/eviction behavior. */
  if (v->ref_count > 1)
  {
    v->ref_count--;
    if (!share->is_view)
      mysql_cond_broadcast(&e->COND_release);
    mysql_mutex_unlock(&e->LOCK_table_share);
    DBUG_VOID_RETURN;
  }
  mysql_mutex_unlock(&e->LOCK_table_share);

  mysql_mutex_lock(&LOCK_unused_shares);
  mysql_mutex_lock(&e->LOCK_table_share);
  if (--v->ref_count)
  {
    if (!share->is_view)
      mysql_cond_broadcast(&e->COND_release);
    mysql_mutex_unlock(&e->LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
  }
  /*
    CURRENT's ref_count just hit 0. We may only put the element on the
    unused_shares LRU or delete it from the hash when the chain holds
    exactly one version (this one). If OLDER versions are still pinned
    (in-use TABLEs from DMLs bound to earlier versions), the element is
    not truly idle and must stay out of the LRU. Some future event — the
    last OLDER version's GC, or a fresh open re-bumping ref_count — will
    bring this element back into a normal state.
  */
  if (e->versions_head != e->versions_tail)
  {
    if (!share->is_view)
      mysql_cond_broadcast(&e->COND_release);
    mysql_mutex_unlock(&e->LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
  }
  if (v->flushed || tdc_records() > tdc_size)
  {
    mysql_mutex_unlock(&LOCK_unused_shares);
    tdc_delete_share_from_hash(e);
    DBUG_VOID_RETURN;
  }
  /* Link share last in used_table_share list */
  DBUG_PRINT("info", ("moving share to unused list"));
  DBUG_ASSERT(e->next == 0);
  unused_shares.push_back(e);
  mysql_mutex_unlock(&e->LOCK_table_share);
  mysql_mutex_unlock(&LOCK_unused_shares);
  DBUG_VOID_RETURN;
}


void tdc_remove_referenced_share(THD *thd, TABLE_SHARE *share)
{
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, share->db.str,
                                             share->table_name.str,
                                             MDL_EXCLUSIVE));
  /*
    The share is about to be deleted from the TDC. Drop our own schema
    binding (if any) so it doesn't dangle past share deletion and so the
    wait/decrement below sees the expected ref_count.
  */
  thd->remove_schema_binding(
    reinterpret_cast<const uchar*>(share->table_cache_key.str),
    share->table_cache_key.length);

  share->tdc->flush_unused(true);
  mysql_mutex_lock(&share->tdc->LOCK_table_share);
  DEBUG_SYNC(thd, "before_wait_for_refs");
  share->tdc->wait_for_refs(1);
  DBUG_ASSERT(share->version->all_tables.is_empty());
  share->version->ref_count--;
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

  if (!element->current()->ref_count)
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

  element->current()->ref_count++;
  mysql_mutex_unlock(&element->LOCK_table_share);

  /* We have to relock the mutex to avoid code duplication. Sigh. */
  tdc_remove_referenced_share(thd, element->current()->share);
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
  else if (element->current()->flushed)
  {
    struct timespec abstime;
    set_timespec(abstime, wait_timeout);
    return element->current()->share->wait_for_old_version(thd, &abstime,
                                                           deadlock_weight);
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


static const uchar *eliminate_duplicates_get_key(const void *element,
                                                 size_t *length, my_bool)
{
  auto key= static_cast<const LEX_STRING *>(element);
  *length= key->length;
  return reinterpret_cast<const uchar *>(key->str);
}


static my_bool eliminate_duplicates(void *el, void *a)
{
  TDC_element *element= static_cast<TDC_element*>(el);
  eliminate_duplicates_arg *arg= static_cast<eliminate_duplicates_arg*>(a);
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
    action= eliminate_duplicates;
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


int show_tc_active_instances(THD *thd, SHOW_VAR *var, void *buff,
                             system_status_var *, enum enum_var_type scope)
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
  while (current()->ref_count > my_refs)
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
  DBUG_ASSERT(current());
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE,
                                             current()->share->db.str,
                                             current()->share->table_name.str,
                                             MDL_EXCLUSIVE));
  const uchar *key=
    reinterpret_cast<const uchar*>(current()->share->table_cache_key.str);
  uint key_len= current()->share->table_cache_key.length;

  /*
    If mark_flushed, the share is going away — drop our binding before
    the drain so the wait doesn't include our binding's pin and so the
    binding doesn't dangle past share deletion.
  */
  if (mark_flushed)
    thd->remove_schema_binding(key, key_len);

  flush_unused(mark_flushed);

  mysql_mutex_lock(&LOCK_table_share);
  /*
    Between the unlock in flush_unused and the re-lock here, a concurrent
    tdc_purge may have picked our element off the unused_shares LRU and
    deleted it (the element's memory persists via LF_HASH hazard pointer
    so the mutex above is still valid, but current() is NULL now). With
    the share already gone there's nothing left to drain or wait for.
  */
  if (!current())
  {
    mysql_mutex_unlock(&LOCK_table_share);
    return;
  }
  All_share_tables_list::Iterator it(current()->all_tables);
  uint my_refs= 0;
  while (auto table= it++)
  {
    if (table->in_use == thd)
      my_refs++;
  }
  /*
    If we didn't drop our binding above, it still pins +1 to ref_count.
    Count it so the wait doesn't hang.
  */
  if (thd->lookup_schema_binding(key, key_len) == current())
    my_refs++;
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
  if (mark_flushed && current())
    current()->flushed= true;
  if (current())
    tc_remove_all_unused_tables(current(), &purge_tables);
  mysql_mutex_unlock(&LOCK_table_share);

  while (auto table= purge_tables.pop_front())
    intern_close_table(table);
}
