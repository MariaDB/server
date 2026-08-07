#ifndef TABLE_CACHE_H_INCLUDED
#define TABLE_CACHE_H_INCLUDED
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


struct Share_free_tables
{
  typedef I_P_List <TABLE, TABLE_share> List;
  List list;
  /** Avoid false sharing between instances */
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];
};


/**
  One schema version of a table.

  TDC_element holds a doubly-linked list of TABLE_SHARE_VERSIONs ordered
  oldest → newest, with versions_tail being the version served to new opens.
  In-flight transactions keep using an older TABLE_SHARE_VERSION while DDL
  appends a newer one as the new tail.

  All per-share TABLE-cache state lives here: every TABLE points to one share,
  and a TABLE_SHARE_VERSION owns the set of TABLEs for its version (both the
  in-use list `all_tables` and the per-cache-instance idle list `free_tables`).
*/

struct TABLE_SHARE_VERSION
{
  /**
    Monotonic version number, allocated from TDC_element::next_schema_version
    at install time. Unique within the lifetime of one TDC_element entry;
    used for ordering and diagnostics. Not persisted, not propagated via
    replication. For cross-server schema fingerprinting see
    TABLE_SHARE::tabledef_version (UUID for tables, timestamp for views).
  */
  uint64_t schema_version;
  TABLE_SHARE *share;
  uint ref_count;                       /* How many TABLE objects use this */
  uint all_tables_refs;                 /* Number of refs to all_tables */
  bool flushed;
  /** Chain links in TDC_element. NULL on a version that hasn't been
      installed yet, or after it's been GC'd. */
  TABLE_SHARE_VERSION *next, *prev;
  /*
    Doubly-linked (back-linked) lists of used and unused TABLE objects
    for this version. Protected by TDC_element::LOCK_table_share and
    (for free_tables[i]) tc[i].LOCK_table_cache.
  */
  All_share_tables_list all_tables;
  /** Avoid false sharing between header fields and free_tables */
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];
  /** Idle TABLE objects per cache instance. Sized to tc_instances at alloc. */
  Share_free_tables free_tables[1];
};


struct TDC_element
{
  uchar m_key[NAME_LEN + 1 + NAME_LEN + 1];
  uint m_key_length;

  /**
    Protects m_flush_tickets, the version chain
    (versions_head/versions_tail/next_schema_version), and the mutable fields
    inside each TABLE_SHARE_VERSION (ref_count, share, flushed, next/prev,
    all_tables, all_tables_refs).
  */
  mysql_mutex_t LOCK_table_share;
  mysql_cond_t COND_release;
  TDC_element *next, **prev;            /* Link to unused shares */
  /**
    List of tickets representing threads waiting for the share to be flushed.
    Per-name, not per-version: a flush wakes up everyone waiting on this name.
  */
  Wait_for_flush_list m_flush_tickets;

  /**
    Version chain. Ordered oldest → newest. versions_tail is the version
    served to new opens (== "current"). NULL when the element is being
    initialized (between lf_hash_insert and the first tdc_install_version).
  */
  TABLE_SHARE_VERSION *versions_head;
  TABLE_SHARE_VERSION *versions_tail;
  /** Per-element monotonic counter for TABLE_SHARE_VERSION::schema_version. */
  uint64_t next_schema_version;

  /** The version served to new opens; NULL if none installed yet. */
  TABLE_SHARE_VERSION *current() const { return versions_tail; }

  inline void wait_for_refs(uint my_refs);
  void flush(THD *thd, bool mark_flushed);
  void flush_unused(bool mark_flushed);
};


extern ulong tdc_size;
extern ulong tc_size;
extern uint32 tc_instances;

extern bool tdc_init(void);
extern void tdc_start_shutdown(bool use_dummy_thd);
extern void tdc_deinit(void);
extern ulong tdc_records(void);
extern void tdc_purge(bool all);
extern TDC_element *tdc_lock_share(THD *thd, const char *db,
                                   const char *table_name);
extern void tdc_unlock_share(TDC_element *element);
int tdc_share_is_cached(THD *thd, const char *db, const char *table_name);
extern TABLE_SHARE *tdc_acquire_share(THD *thd, TABLE_LIST *tl, uint flags,
                                      TABLE **out_table= 0);
extern void tdc_release_share(TABLE_SHARE *share);
void tdc_remove_referenced_share(THD *thd, TABLE_SHARE *share);
void tdc_remove_table(THD *thd, const char *db, const char *table_name);

extern int tdc_wait_for_old_version(THD *thd, const char *db,
                                    const char *table_name,
                                    ulong wait_timeout, uint deadlock_weight);
extern int tdc_iterate(THD *thd, my_hash_walk_action action, void *argument,
                       bool no_dups= false);

extern uint tc_records(void);
int show_tc_active_instances(THD *thd, SHOW_VAR *var, void *buff,
                             system_status_var *, enum enum_var_type scope);
extern void tc_purge();
extern void tc_add_table(THD *thd, TABLE *table);
extern void tc_release_table(TABLE *table);
extern TABLE *tc_acquire_table(THD *thd, TABLE_SHARE_VERSION *version);

/*
  Multi-version TDC helpers (used by lock-free DDL paths).
  Callers must hold LOCK_table_share for tdc_install_version and
  tdc_gc_version. tdc_alloc_version/tdc_free_version do their own
  allocation without locks.
*/
extern TABLE_SHARE_VERSION *tdc_alloc_version();
extern void tdc_free_version(TABLE_SHARE_VERSION *v);
extern void tdc_install_version(TDC_element *e, TABLE_SHARE_VERSION *v);
extern void tdc_gc_version(TDC_element *e, TABLE_SHARE_VERSION *v);

/**
  Create a table cache key for non-temporary table.

  @param key         Buffer for key (must be at least NAME_LEN*2+2 bytes).
  @param db          Database name.
  @param table_name  Table name.

  @return Length of key.
*/

inline uint tdc_create_key(char *key, const char *db, const char *table_name)
{
  /*
    In theory caller should ensure that both db and table_name are
    not longer than NAME_LEN bytes. In practice we play safe to avoid
    buffer overruns.
  */
  return (uint) (strmake(strmake(key, db, NAME_LEN) + 1, table_name,
                         NAME_LEN) - key + 1);
}
#endif /* TABLE_CACHE_H_INCLUDED */
