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


#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_TABLE_SHARE_LOCK_table_share;
extern PSI_cond_key key_TABLE_SHARE_COND_release;
#endif

class TDC_element
{
public:
  uchar m_key[NAME_LEN + 1 + NAME_LEN + 1];
  uint m_key_length;
  ulong version;
  bool flushed;
  TABLE_SHARE *share;

  typedef I_P_List <TABLE, TABLE_share> TABLE_list;
  typedef I_P_List <TABLE, All_share_tables> All_share_tables_list;
  /**
    Protects ref_count, m_flush_tickets, all_tables, free_tables, flushed,
    all_tables_refs.
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
  TABLE_list free_tables;

  TDC_element() {}

  TDC_element(const char *key_arg, uint key_length) : m_key_length(key_length)
  {
    memcpy(m_key, key_arg, key_length);
  }


  void assert_clean_share()
  {
    DBUG_ASSERT(share == 0);
    DBUG_ASSERT(ref_count == 0);
    DBUG_ASSERT(m_flush_tickets.is_empty());
    DBUG_ASSERT(all_tables.is_empty());
    DBUG_ASSERT(free_tables.is_empty());
    DBUG_ASSERT(all_tables_refs == 0);
    DBUG_ASSERT(next == 0);
    DBUG_ASSERT(prev == 0);
  }


  /**
    Acquire TABLE object from table cache.

    @pre share must be protected against removal.

    Acquired object cannot be evicted or acquired again.

    @return TABLE object, or NULL if no unused objects.
  */

  TABLE *acquire_table(THD *thd)
  {
    TABLE *table;

    mysql_mutex_lock(&LOCK_table_share);
    table= free_tables.pop_front();
    if (table)
    {
      DBUG_ASSERT(!table->in_use);
      table->in_use= thd;
      /* The ex-unused table must be fully functional. */
      DBUG_ASSERT(table->db_stat && table->file);
      /* The children must be detached from the table. */
      DBUG_ASSERT(!table->file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
    }
    mysql_mutex_unlock(&LOCK_table_share);
    return table;
  }


  /**
    Get last element of free_tables.
  */

  TABLE *free_tables_back()
  {
    TABLE_list::Iterator it(free_tables);
    TABLE *entry, *last= 0;
     while ((entry= it++))
       last= entry;
    return last;
  }


  /**
    Wait for MDL deadlock detector to complete traversing tdc.all_tables.

    Must be called before updating TABLE_SHARE::tdc.all_tables.
  */

  void wait_for_mdl_deadlock_detector()
  {
    while (all_tables_refs)
      mysql_cond_wait(&COND_release, &LOCK_table_share);
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
    element->free_tables.empty();
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
    element->assert_clean_share();
    mysql_cond_destroy(&element->COND_release);
    mysql_mutex_destroy(&element->LOCK_table_share);
    DBUG_VOID_RETURN;
  }


  static void lf_hash_initializer(LF_HASH *hash __attribute__((unused)),
                                  TDC_element *element, LEX_STRING *key)
  {
    memcpy(element->m_key, key->str, key->length);
    element->m_key_length= key->length;
    element->assert_clean_share();
  }


  static uchar *key(const TDC_element *element, size_t *length,
                    my_bool not_used __attribute__((unused)))
  {
    *length= element->m_key_length;
    return (uchar*) element->m_key;
  }
};


enum enum_tdc_remove_table_type
{
  TDC_RT_REMOVE_ALL,
  TDC_RT_REMOVE_NOT_OWN,
  TDC_RT_REMOVE_UNUSED,
  TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE
};

extern ulong tdc_size;
extern ulong tc_size;

extern void tdc_init(void);
extern void tdc_start_shutdown(void);
extern void tdc_deinit(void);
extern ulong tdc_records(void);
extern void tdc_purge(bool all);
extern TDC_element *tdc_lock_share(THD *thd, const char *db,
                                   const char *table_name);
extern void tdc_unlock_share(TDC_element *element);
extern TABLE_SHARE *tdc_acquire_share(THD *thd, const char *db,
                                      const char *table_name,
                                      const char *key, uint key_length,
                                      my_hash_value_type hash_value,
                                      uint flags, TABLE **out_table);
extern void tdc_release_share(TABLE_SHARE *share);
extern bool tdc_remove_table(THD *thd, enum_tdc_remove_table_type remove_type,
                             const char *db, const char *table_name,
                             bool kill_delayed_threads);
extern int tdc_wait_for_old_version(THD *thd, const char *db,
                                    const char *table_name,
                                    ulong wait_timeout, uint deadlock_weight,
                                    ulong refresh_version= ULONG_MAX);
extern ulong tdc_refresh_version(void);
extern ulong tdc_increment_refresh_version(void);
extern void tdc_assign_new_table_id(TABLE_SHARE *share);
extern int tdc_iterate(THD *thd, my_hash_walk_action action, void *argument,
                       bool no_dups= false);

extern uint tc_records(void);
extern void tc_purge(bool mark_flushed= false);
extern void tc_add_table(THD *thd, TABLE *table);
extern bool tc_release_table(TABLE *table);

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

/**
  Convenience helper: call tdc_acquire_share() without out_table.
*/

static inline TABLE_SHARE *tdc_acquire_share(THD *thd, const char *db,
                                             const char *table_name,
                                             const char *key,
                                             uint key_length, uint flags)
{
  return tdc_acquire_share(thd, db, table_name, key, key_length,
                           my_hash_sort(&my_charset_bin, (uchar*) key,
                                        key_length), flags, 0);
}


/**
  Convenience helper: call tdc_acquire_share() without precomputed cache key.
*/

static inline TABLE_SHARE *tdc_acquire_share(THD *thd, const char *db,
                                             const char *table_name, uint flags)
{
  char key[MAX_DBKEY_LENGTH];
  uint key_length;
  key_length= tdc_create_key(key, db, table_name);
  return tdc_acquire_share(thd, db, table_name, key, key_length, flags);
}


/**
  Convenience helper: call tdc_acquire_share() reusing the MDL cache key.

  @note lifetime of the returned TABLE_SHARE is limited by the
        lifetime of the TABLE_LIST object!!!
*/

uint get_table_def_key(const TABLE_LIST *table_list, const char **key);

static inline TABLE_SHARE *tdc_acquire_share_shortlived(THD *thd, TABLE_LIST *tl,
                                                        uint flags)
{
  const char *key;
  uint        key_length= get_table_def_key(tl, &key);
  return tdc_acquire_share(thd, tl->db, tl->table_name, key, key_length,
                           tl->mdl_request.key.tc_hash_value(), flags, 0);
}
