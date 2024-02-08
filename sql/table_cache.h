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


struct TDC_element
{
  uchar m_key[NAME_LEN + 1 + NAME_LEN + 1];
  uint m_key_length;
  bool flushed;
  TABLE_SHARE *share;

  /**
    Protects ref_count, m_flush_tickets, all_tables, flushed, all_tables_refs.
  */
  mysql_mutex_t LOCK_table_share;
  mysql_cond_t COND_release;
  TDC_element *next, **prev;            /* Link to unused shares */
  uint ref_count;                       /* How many TABLE objects uses this */
  uint all_tables_refs;                 /* Number of refs to all_tables */
  /**
    List of tickets representing threads waiting for the share to be flushed.
  */
  Wait_for_flush_list m_flush_tickets;
  /*
    Doubly-linked (back-linked) lists of used and unused TABLE objects
    for this share.
  */
  All_share_tables_list all_tables;
  /** Avoid false sharing between TDC_element and free_tables */
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];
  Share_free_tables free_tables[1];

  inline void wait_for_refs(uint my_refs);
  void flush(THD *thd, bool mark_flushed);
  void flush_unused(bool mark_flushed);
};


extern ulong tdc_size;
extern ulong tc_size;
extern uint32 tc_instances;

extern bool tdc_init(void);
extern void tdc_start_shutdown(void);
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
extern TABLE *tc_acquire_table(THD *thd, TDC_element *element);

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
