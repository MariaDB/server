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


enum enum_tdc_remove_table_type
{
  TDC_RT_REMOVE_ALL,
  TDC_RT_REMOVE_NOT_OWN,
  TDC_RT_REMOVE_UNUSED,
  TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE
};

extern ulong tdc_size;
extern ulong tc_size;

extern int tdc_init(void);
extern void tdc_start_shutdown(void);
extern void tdc_deinit(void);
extern ulong tdc_records(void);
extern void tdc_purge(bool all);
extern void tdc_init_share(TABLE_SHARE *share);
extern void tdc_deinit_share(TABLE_SHARE *share);
extern TABLE_SHARE *tdc_lock_share(const char *db, const char *table_name);
extern void tdc_unlock_share(TABLE_SHARE *share);
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


class TDC_iterator
{
  ulong idx;
public:
  void init(void);
  void deinit(void);
  TABLE_SHARE *next(void);
};
