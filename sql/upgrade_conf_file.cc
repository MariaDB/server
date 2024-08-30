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
#include <my_global.h>
#include <my_sys.h>
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
"innodb_adaptive_max_sleep_delay",
"innodb_additional_mem_pool_size",
"innodb_api_bk_commit_interval",
"innodb_api_disable_rowlock",
"innodb_api_enable_binlog",
"innodb_api_enable_mdl",
"innodb_api_trx_level",
"innodb_background_scrub_data_check_interval",
"innodb_background_scrub_data_compressed",
"innodb_background_scrub_data_interval",
"innodb_background_scrub_data_uncompressed",
"innodb_blocking_buffer_pool_restore",
"innodb_buffer_pool_instances",
"innodb_buffer_pool_populate",
"innodb_buffer_pool_restore_at_startup",
"innodb_buffer_pool_shm_checksum",
"innodb_buffer_pool_shm_key",
"innodb_checkpoint_age_target",
"innodb_checksums",
"innodb_cleaner_eviction_factor",
"innodb_cleaner_flush_chunk_size",
"innodb_cleaner_free_list_lwm",
"innodb_cleaner_lru_chunk_size",
"innodb_cleaner_lsn_age_factor",
"innodb_cleaner_max_flush_time",
"innodb_cleaner_max_lru_time",
"innodb_commit_concurrency",
"innodb_concurrency_tickets",
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
"innodb_force_load_corrupted",
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
"innodb_locks_unsafe_for_binlog",
"innodb_log_arch_dir",
"innodb_log_arch_expire_sec",
"innodb_log_archive",
"innodb_log_block_size",
"innodb_log_checksum_algorithm",
"innodb_log_checksums",
"innodb_log_compressed_pages",
"innodb_log_files_in_group",
"innodb_log_optimize_ddl",
"innodb_max_bitmap_file_size",
"innodb_max_changed_pages",
"innodb_merge_sort_block_size",
"innodb_mirrored_log_groups",
"innodb_mtflush_threads",
"innodb_page_cleaners",
"innodb_persistent_stats_root_page",
"innodb_print_lock_wait_timeout_info",
"innodb_purge_run_now",
"innodb_purge_stop_now",
"innodb_read_ahead",
"innodb_recovery_stats",
"innodb_recovery_update_relay_log",
"innodb_replication_delay",
"innodb_rollback_segments",
"innodb_scrub_log",
"innodb_scrub_log_speed",
"innodb_show_locks_held",
"innodb_show_verbose_locks",
"innodb_stats_auto_update",
"innodb_stats_sample_pages",
"innodb_stats_update_need_lock",
"innodb_support_xa",
"innodb_sync_array_size",
"innodb_thread_concurrency",
"innodb_thread_concurrency_timer_based",
"innodb_thread_sleep_delay",
"innodb_track_changed_pages",
"innodb_track_redo_log_now",
"innodb_undo_logs",
"innodb_use_fallocate",
"innodb_use_global_flush_log_at_trx_commit",
"innodb_use_mtflush",
"innodb_use_stacktrace",
"innodb_use_sys_malloc",
"innodb_use_sys_stats_table",
"innodb_use_trim",
"log",
"log_slow_queries",
"max_long_data_size",
"multi_range_count",
"rpl_recovery_rank",
"skip_bdb",
"sql_big_tables",
"sql_low_priority_updates",
"sql_max_join_size",
"thread_concurrency",
"timed_mutexes"
};


static int cmp_strings(const void* a, const void *b)
{
  return strcmp((const char *)a, *(const char **)b);
}


#define MY_INI_SECTION_SIZE 32 * 1024 + 3

static bool is_utf8_str(const char *s)
{
  MY_STRCOPY_STATUS status;
  const struct charset_info_st *cs= &my_charset_utf8mb4_bin;
  size_t len= strlen(s);
  if (!len)
    return true;
  cs->cset->well_formed_char_length(cs, s, s + len, len, &status);
  return status.m_well_formed_error_pos == nullptr;
}


static UINT get_system_acp()
{
  static DWORD system_acp;
  if (system_acp)
    return system_acp;

  char str_cp[10];
  int cch= GetLocaleInfo(GetSystemDefaultLCID(), LOCALE_IDEFAULTANSICODEPAGE,
                         str_cp, sizeof(str_cp));

  system_acp= cch > 0 ? atoi(str_cp) : 1252;

  return system_acp;
}


static char *ansi_to_utf8(const char *s)
{
#define MAX_STR_LEN MY_INI_SECTION_SIZE
  static wchar_t utf16_buf[MAX_STR_LEN];
  static char utf8_buf[MAX_STR_LEN];
  if (MultiByteToWideChar(get_system_acp(), 0, s, -1, utf16_buf, MAX_STR_LEN))
  {
    if (WideCharToMultiByte(CP_UTF8, 0, utf16_buf, -1, utf8_buf, MAX_STR_LEN,
                            0, 0))
      return utf8_buf;
  }
  return 0;
}

int fix_section(const char *myini_path, const char *section_name,
                bool is_server)
{
  if (!is_server && GetACP() != CP_UTF8)
    return 0;

  static char section_data[MY_INI_SECTION_SIZE];
  DWORD size= GetPrivateProfileSection(section_name, section_data,
                                       MY_INI_SECTION_SIZE, myini_path);
  if (size == MY_INI_SECTION_SIZE - 2)
  {
    return -1;
  }

  for (char *keyval= section_data; *keyval; keyval += strlen(keyval)+1)
  {
    char varname[256];
    char *value;
    char *key_end= strchr(keyval, '=');
    if (!key_end)
      key_end= keyval + strlen(keyval);

    if (key_end - keyval > sizeof(varname))
      continue;

    value= key_end + 1;
    if (GetACP() == CP_UTF8 && !is_utf8_str(value))
    {
      /*Convert a value, if it is not already UTF-8*/
      char *new_val= ansi_to_utf8(value);
      if (new_val)
      {
        *key_end= 0;
        fprintf(stdout, "Fixing variable '%s' charset, value=%s\n", keyval,
                new_val);
        WritePrivateProfileString(section_name, keyval, new_val, myini_path);
        *key_end= '=';
      }
    }
    if (!is_server)
      continue;

    // Check if variable should be removed from config.
    // First, copy and normalize (convert dash to underscore) to  variable
    // names
    for (char *p= keyval, *q= varname;; p++, q++)
    {
      if (p == key_end)
      {
        *q= 0;
        break;
      }
      *q= (*p == '-') ? '_' : *p;
    }
    const char *v= (const char *) bsearch(varname, removed_variables, sizeof(removed_variables) / sizeof(removed_variables[0]),
                                          sizeof(char *), cmp_strings);

    if (v)
    {
      fprintf(stdout, "Removing variable '%s' from config file\n", varname);
      // delete variable
      *key_end= 0;
      WritePrivateProfileString(section_name, keyval, 0, myini_path);
    }
  }
  return 0;
}

static bool is_mariadb_section(const char *name, bool *is_server)
{
  if (strncmp(name, "mysql", 5)
      && strncmp(name, "mariadb", 7)
      && strcmp(name, "client")
      && strcmp(name, "client-server")
      && strcmp(name, "server"))
  {
    return false;
  }

  for (const char *section_name : {"mysqld", "server", "mariadb"})
    if (*is_server= !strcmp(section_name, name))
      break;

  return true;
}


/**
  Convert file from a previous version, by removing obsolete variables
  Also, fix values to be UTF8, if MariaDB is running in utf8 mode
*/
int upgrade_config_file(const char *myini_path)
{
  static char all_sections[MY_INI_SECTION_SIZE];
  int sz= GetPrivateProfileSectionNamesA(all_sections, MY_INI_SECTION_SIZE,
                                         myini_path);
  if (!sz)
    return 0;
  if (sz > MY_INI_SECTION_SIZE - 2)
  {
    fprintf(stderr, "Too many sections in config file\n");
    return -1;
  }
  for (char *section= all_sections; *section; section+= strlen(section) + 1)
  {
    bool is_server_section;
    if (is_mariadb_section(section, &is_server_section))
      fix_section(myini_path, section, is_server_section);
  }
  return 0;
}
