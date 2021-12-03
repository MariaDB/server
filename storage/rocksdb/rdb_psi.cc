/* Copyright (c) 2017, Percona and/or its affiliates. All rights reserved.

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation  // gcc: Class implementation
#endif

#define MYSQL_SERVER 1

/* The C++ file's header */
#include "./rdb_psi.h"

namespace myrocks {

/*
  The following is needed as an argument for mysql_stage_register,
  irrespectively of whether we're compiling with P_S or not.
*/
my_core::PSI_stage_info stage_waiting_on_row_lock = {0, "Waiting for row lock",
                                                     0};

#ifdef HAVE_PSI_INTERFACE
my_core::PSI_stage_info *all_rocksdb_stages[] = {&stage_waiting_on_row_lock};

my_core::PSI_thread_key rdb_background_psi_thread_key,
    rdb_drop_idx_psi_thread_key, rdb_mc_psi_thread_key;

my_core::PSI_thread_info all_rocksdb_threads[] = {
    {&rdb_background_psi_thread_key, "background", PSI_FLAG_GLOBAL},
    {&rdb_drop_idx_psi_thread_key, "drop index", PSI_FLAG_GLOBAL},
    {&rdb_mc_psi_thread_key, "manual compaction", PSI_FLAG_GLOBAL},
};

my_core::PSI_mutex_key rdb_psi_open_tbls_mutex_key, rdb_signal_bg_psi_mutex_key,
    rdb_signal_drop_idx_psi_mutex_key, rdb_signal_mc_psi_mutex_key,
    rdb_collation_data_mutex_key, rdb_mem_cmp_space_mutex_key,
    key_mutex_tx_list, rdb_sysvars_psi_mutex_key, rdb_cfm_mutex_key,
    rdb_sst_commit_key, rdb_block_cache_resize_mutex_key;

my_core::PSI_mutex_info all_rocksdb_mutexes[] = {
    {&rdb_psi_open_tbls_mutex_key, "open tables", PSI_FLAG_GLOBAL},
    {&rdb_signal_bg_psi_mutex_key, "stop background", PSI_FLAG_GLOBAL},
    {&rdb_signal_drop_idx_psi_mutex_key, "signal drop index", PSI_FLAG_GLOBAL},
    {&rdb_signal_mc_psi_mutex_key, "signal manual compaction", PSI_FLAG_GLOBAL},
    {&rdb_collation_data_mutex_key, "collation data init", PSI_FLAG_GLOBAL},
    {&rdb_mem_cmp_space_mutex_key, "collation space char data init",
     PSI_FLAG_GLOBAL},
    {&key_mutex_tx_list, "tx_list", PSI_FLAG_GLOBAL},
    {&rdb_sysvars_psi_mutex_key, "setting sysvar", PSI_FLAG_GLOBAL},
    {&rdb_cfm_mutex_key, "column family manager", PSI_FLAG_GLOBAL},
    {&rdb_sst_commit_key, "sst commit", PSI_FLAG_GLOBAL},
    {&rdb_block_cache_resize_mutex_key, "resizing block cache",
     PSI_FLAG_GLOBAL},
};

my_core::PSI_rwlock_key key_rwlock_collation_exception_list,
    key_rwlock_read_free_rpl_tables, key_rwlock_skip_unique_check_tables;

my_core::PSI_rwlock_info all_rocksdb_rwlocks[] = {
    {&key_rwlock_collation_exception_list, "collation_exception_list",
     PSI_FLAG_GLOBAL},
    {&key_rwlock_read_free_rpl_tables, "read_free_rpl_tables", PSI_FLAG_GLOBAL},
    {&key_rwlock_skip_unique_check_tables, "skip_unique_check_tables",
     PSI_FLAG_GLOBAL},
};

my_core::PSI_cond_key rdb_signal_bg_psi_cond_key,
    rdb_signal_drop_idx_psi_cond_key, rdb_signal_mc_psi_cond_key;

my_core::PSI_cond_info all_rocksdb_conds[] = {
    {&rdb_signal_bg_psi_cond_key, "cond signal background", PSI_FLAG_GLOBAL},
    {&rdb_signal_drop_idx_psi_cond_key, "cond signal drop index",
     PSI_FLAG_GLOBAL},
    {&rdb_signal_mc_psi_cond_key, "cond signal manual compaction",
     PSI_FLAG_GLOBAL},
};

void init_rocksdb_psi_keys() {
  const char *const category = "rocksdb";
  int count;

  count = array_elements(all_rocksdb_mutexes);
  mysql_mutex_register(category, all_rocksdb_mutexes, count);

  count = array_elements(all_rocksdb_rwlocks);
  mysql_rwlock_register(category, all_rocksdb_rwlocks, count);

  count = array_elements(all_rocksdb_conds);
  // TODO(jay) Disabling PFS for conditions due to the bug
  // https://github.com/MySQLOnRocksDB/mysql-5.6/issues/92
  // PSI_server->register_cond(category, all_rocksdb_conds, count);

  count = array_elements(all_rocksdb_stages);
  mysql_stage_register(category, all_rocksdb_stages, count);

  count = array_elements(all_rocksdb_threads);
  mysql_thread_register(category, all_rocksdb_threads, count);
}
#else   // HAVE_PSI_INTERFACE
void init_rocksdb_psi_keys() {}
#endif  // HAVE_PSI_INTERFACE

}  // namespace myrocks
