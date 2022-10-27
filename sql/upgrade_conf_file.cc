/*
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */


/*
  Variables that were present in older releases, but are now removed.
  to get the list of variables that are present in current release
  execute

  SELECT LOWER(variable_name) from INFORMATION_SCHEMA.GLOBAL_VARIABLES ORDER BY 1

  Compare the list between releases to figure out which variables have gone.

  Note : the list below only includes the default-compiled server and none of the
  loadable plugins.
*/
#include <windows.h>
#include <initializer_list>
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>

static const char *removed_variables[] =
{
"aria_recover",
"debug_crc_break",
"engine_condition_pushdown",
"have_csv",
"have_innodb",
"have_ndbcluster",
"have_partitioning",
"innodb_adaptive_flushing_method",
"innodb_adaptive_hash_index_partitions",
"innodb_additional_mem_pool_size",
"innodb_api_bk_commit_interval",
"innodb_api_disable_rowlock",
"innodb_api_enable_binlog",
"innodb_api_enable_mdl",
"innodb_api_trx_level",
"innodb_blocking_buffer_pool_restore",
"innodb_buffer_pool_populate",
"innodb_buffer_pool_restore_at_startup",
"innodb_buffer_pool_shm_checksum",
"innodb_buffer_pool_shm_key",
"innodb_checkpoint_age_target",
"innodb_cleaner_eviction_factor",
"innodb_cleaner_flush_chunk_size",
"innodb_cleaner_free_list_lwm",
"innodb_cleaner_lru_chunk_size",
"innodb_cleaner_lsn_age_factor",
"innodb_cleaner_max_flush_time",
"innodb_cleaner_max_lru_time",
"innodb_corrupt_table_action",
"innodb_dict_size_limit",
"innodb_doublewrite_file",
"innodb_empty_free_list_algorithm",
"innodb_fake_changes",
"innodb_fast_checksum",
"innodb_file_format",
"innodb_file_format_check",
"innodb_file_format_max",
"innodb_flush_neighbor_pages",
"innodb_foreground_preflush",
"innodb_ibuf_accel_rate",
"innodb_ibuf_active_contract",
"innodb_ibuf_max_size",
"innodb_idle_flush_pct",
"innodb_import_table_from_xtrabackup",
"innodb_instrument_semaphores",
"innodb_kill_idle_transaction",
"innodb_large_prefix",
"innodb_lazy_drop_table",
"innodb_locking_fake_changes",
"innodb_log_arch_dir",
"innodb_log_arch_expire_sec",
"innodb_log_archive",
"innodb_log_block_size",
"innodb_log_checksum_algorithm",
"innodb_max_bitmap_file_size",
"innodb_max_changed_pages",
"innodb_merge_sort_block_size",
"innodb_mirrored_log_groups",
"innodb_mtflush_threads",
"innodb_persistent_stats_root_page",
"innodb_print_lock_wait_timeout_info",
"innodb_purge_run_now",
"innodb_purge_stop_now",
"innodb_read_ahead",
"innodb_recovery_stats",
"innodb_recovery_update_relay_log",
"innodb_show_locks_held",
"innodb_show_verbose_locks",
"innodb_stats_auto_update",
"innodb_stats_update_need_lock",
"innodb_support_xa",
"innodb_thread_concurrency_timer_based",
"innodb_track_changed_pages",
"innodb_track_redo_log_now",
"innodb_use_fallocate",
"innodb_use_global_flush_log_at_trx_commit",
"innodb_use_mtflush",
"innodb_use_stacktrace",
"innodb_use_sys_malloc",
"innodb_use_sys_stats_table",
"innodb_use_trim",
"log",
"log_slow_queries",
"rpl_recovery_rank",
"sql_big_tables",
"sql_low_priority_updates",
"sql_max_join_size"
};


static int cmp_strings(const void* a, const void *b)
{
  return strcmp((const char *)a, *(const char **)b);
}

/**
  Convert file from a previous version, by removing
*/
int upgrade_config_file(const char *myini_path)
{
#define MY_INI_SECTION_SIZE 32*1024 +3
  static char section_data[MY_INI_SECTION_SIZE];
  for (const char *section_name : { "mysqld","server","mariadb" })
  {
    DWORD size = GetPrivateProfileSection(section_name, section_data, MY_INI_SECTION_SIZE, myini_path);
    if (size == MY_INI_SECTION_SIZE - 2)
    {
      return -1;
    }

    for (char *keyval = section_data; *keyval; keyval += strlen(keyval) + 1)
    {
      char varname[256];
      char *key_end = strchr(keyval, '=');
      if (!key_end)
        key_end = keyval+ strlen(keyval);

      if (key_end - keyval > sizeof(varname))
        continue;
      // copy and normalize (convert dash to underscore) to  variable names
      for (char *p = keyval, *q = varname;; p++,q++)
      {
        if (p == key_end)
        {
          *q = 0;
          break;
        }
        *q = (*p == '-') ? '_' : *p;
      }
      const char *v = (const char *)bsearch(varname, removed_variables, sizeof(removed_variables) / sizeof(removed_variables[0]),
        sizeof(char *), cmp_strings);

      if (v)
      {
        fprintf(stdout, "Removing variable '%s' from config file\n", varname);
        // delete variable
        *key_end = 0;
        WritePrivateProfileString(section_name, keyval, 0, myini_path);
      }
    }
  }
  return 0;
}
