/* Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA */

/**
  @file extra/mariadb_frm.cc
  FRM file parser utility - extracts table structure from .frm files
*/

#include "mariadb.h"

#include "mysqld.h"
#include "sql_class.h"
#include "table.h"
#include "sql_table.h"
#include "sql_parse.h"
#include "sql_plugin.h"
#include <my_dir.h>
#include "field.h"

extern "C" {
  struct st_maria_plugin *mysql_mandatory_plugins[] = { NULL };
  struct st_maria_plugin *mysql_optional_plugins[] = { NULL };
}


static handlerton mock_hton;

static st_maria_plugin mock_plugin = {
  .type = MYSQL_STORAGE_ENGINE_PLUGIN,
  .info = &mock_hton,
  .name = "MOCK_ENGINE",
  .author = "Mock Author",
  .descr = "Mock storage engine for FRM parsing",
  .license = PLUGIN_LICENSE_GPL,
  .init = NULL,
  .deinit = NULL,
  .version = 0x0100,
  .status_vars = NULL,
  .system_vars = NULL,
  .version_info = "1.0",
  .maturity = MariaDB_PLUGIN_MATURITY_STABLE
};

static st_plugin_int mock_plugin_int = {
  .name = {const_cast<char*>("mock_storage_engine"), 19},
  .plugin = &mock_plugin,
  .plugin_dl = NULL,
  .state = PLUGIN_IS_READY,
  .ref_count = 1,
  .load_option = PLUGIN_ON,
  .locks_total = 0,
  .mem_root = {0},
  .system_vars = NULL,
  .nbackups = 0,
  .ptr_backup = NULL
};

static plugin_ref mock_plugin_ref = (plugin_ref)&mock_plugin_int;

extern mysql_mutex_t LOCK_plugin;
static bool plugin_system_initialized = false;

static void init_minimal_plugin_system()
{
  if (plugin_system_initialized)
    return;
    
  printf("DEBUG: Initializing minimal plugin system\n");
  fflush(stdout);
  
  plugin_mutex_init();
  
  plugin_system_initialized = true;
  
  printf("DEBUG: Plugin system initialized\n");
  fflush(stdout);
}

static void cleanup_minimal_plugin_system()
{
  if (plugin_system_initialized) {
    mysql_mutex_destroy(&LOCK_plugin);
    plugin_system_initialized = false;
  }
}

#define ha_resolve_by_name(thd, name, tmp) (mock_plugin_ref)
#define ha_checktype(thd, type, check) (&mock_hton)
#define ha_lock_engine(thd, hton) (mock_plugin_ref)
#define plugin_hton(plugin) (&mock_hton)
#define get_new_handler(share, root, hton) (NULL)

#define plugin_lock_by_name(thd, name, type) (mock_plugin_ref)
#define plugin_find_by_type(type) (mock_plugin_ref)
#define ha_resolve_by_legacy_type(thd, type) (mock_plugin_ref)
#define plugin_find_internal(name, type) (&mock_plugin_int)
#define plugin_int_to_ref(plugin) (mock_plugin_ref)
#define plugin_ref_to_int(plugin) (&mock_plugin_int)

#define ha_default_handlerton(thd) (&mock_hton)
#define ha_storage_engine_is_enabled(hton) (true)

#define enum_value_with_check(thd, share, name, value, max_val) ((value <= max_val) ? value : 0)

#define status_var_increment(x) do { } while(0)

static plugin_ref mock_plugin_lock(THD *thd, plugin_ref ptr) {
  printf("DEBUG: mock_plugin_lock called with ptr=%p\n", ptr);
  fflush(stdout);
  return ptr ? ptr : mock_plugin_ref;
}

static void mock_plugin_unlock(THD *thd, plugin_ref ptr) {
  printf("DEBUG: mock_plugin_unlock called with ptr=%p\n", ptr);
  fflush(stdout);
}

#define plugin_lock(thd, ptr) mock_plugin_lock(thd, ptr)
#define plugin_unlock(thd, ptr) mock_plugin_unlock(thd, ptr)

/**
  Fake THD structure that contains only fields needed for FRM parsing
*/
struct FakeTHD {
  MEM_ROOT mem_root;
  char *thread_stack;
  
  struct {
    const CHARSET_INFO *character_set_client;
    const CHARSET_INFO *collation_connection;
    const CHARSET_INFO *collation_database;
    const CHARSET_INFO *character_set_results;
  } variables;
  
  struct {
    ulong feature_system_versioning;
    ulong feature_application_time_periods;
  } status_var;
};

/**
  Initialize fake THD structure
*/
static FakeTHD* init_fake_thd()
{
  printf("DEBUG: Entering init_fake_thd\n");
  fflush(stdout);

  init_minimal_plugin_system();

  FakeTHD *fake_thd = (FakeTHD*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(FakeTHD), MYF(MY_ZEROFILL));
  if (!fake_thd)
    return NULL;
    
  init_alloc_root(PSI_NOT_INSTRUMENTED, &fake_thd->mem_root, 8192, 0, MYF(0));
  
  char stack_dummy;
  fake_thd->thread_stack = &stack_dummy;
  
  fake_thd->variables.character_set_client = &my_charset_utf8mb4_general_ci;
  fake_thd->variables.collation_connection = &my_charset_utf8mb4_general_ci;
  fake_thd->variables.collation_database = &my_charset_utf8mb4_general_ci;
  fake_thd->variables.character_set_results = &my_charset_utf8mb4_general_ci;
  
  fake_thd->status_var.feature_system_versioning = 0;
  fake_thd->status_var.feature_application_time_periods = 0;

  printf("DEBUG: Fake THD initialized successfully\n");
  fflush(stdout);
  return fake_thd;
}

/**
  Cleanup fake THD
*/
static void cleanup_fake_thd(FakeTHD *fake_thd)
{
  if (fake_thd)
  {
    free_root(&fake_thd->mem_root, MYF(0));
    my_free(fake_thd);
  }
  
  cleanup_minimal_plugin_system();
}

/**
  Read FRM file into memory
*/
static uchar* read_frm_file(const char *filename, size_t *length)
{
  File file;
  MY_STAT stat_info;
  uchar *buffer= NULL;
  
  if (my_stat(filename, &stat_info, MYF(0)) == NULL)
  {
    fprintf(stderr, "Error: Cannot stat file '%s': %s\n", 
            filename, strerror(errno));
    return NULL;
  }
  
  *length= stat_info.st_size;
  
  if (!(buffer= (uchar*)my_malloc(PSI_NOT_INSTRUMENTED, *length, MYF(MY_WME))))
  {
    fprintf(stderr, "Error: Cannot allocate memory for FRM file\n");
    return NULL;
  }
  
  if ((file= mysql_file_open(key_file_frm, filename, O_RDONLY | O_SHARE, MYF(0))) < 0)
  {
    fprintf(stderr, "Error: Cannot open file '%s': %s\n", 
            filename, strerror(errno));
    my_free(buffer);
    return NULL;
  }
  
  if (mysql_file_read(file, buffer, *length, MYF(MY_NABP)))
  {
    fprintf(stderr, "Error: Cannot read file '%s': %s\n", 
            filename, strerror(errno));
    my_free(buffer);
    mysql_file_close(file, MYF(0));
    return NULL;
  }
  
  mysql_file_close(file, MYF(0));
  return buffer;
}

/**
  Extract database and table name from FRM file path
*/
static bool extract_db_table_names(const char *frm_path, 
                                   LEX_CSTRING *db_name, 
                                   LEX_CSTRING *table_name)
{
  char *path_copy, *db_start, *table_start, *frm_ext;
  
  if (!((path_copy= my_strdup(PSI_NOT_INSTRUMENTED, frm_path, MYF(MY_WME)))))
    return true;
  
  if (!((frm_ext= strstr(path_copy, ".frm"))))
  {
    my_free(path_copy);
    return true;
  }
  *frm_ext= '\0';
  
  table_start= strrchr(path_copy, '/');
  if (!table_start)
    table_start= strrchr(path_copy, '\\');
  
  if (!table_start)
  {
    table_name->str= my_strdup(PSI_NOT_INSTRUMENTED, path_copy, MYF(MY_WME));
    table_name->length= strlen(table_name->str);
    db_name->str= my_strdup(PSI_NOT_INSTRUMENTED, "test", MYF(MY_WME));
    db_name->length= 4;
    my_free(path_copy);
    return false;
  }
  
  *table_start= '\0';
  table_start++;
  
  db_start= strrchr(path_copy, '/');
  if (!db_start)
    db_start= strrchr(path_copy, '\\');
  
  if (!db_start)
    db_start= path_copy;
  else
    db_start++;
  
  table_name->str= my_strdup(PSI_NOT_INSTRUMENTED, table_start, MYF(MY_WME));
  table_name->length= strlen(table_name->str);
  db_name->str= my_strdup(PSI_NOT_INSTRUMENTED, db_start, MYF(MY_WME));
  db_name->length= strlen(db_name->str);
  
  my_free(path_copy);
  return false;
}

/**
  Parse FRM file and create TABLE_SHARE and TABLE structures
*/
static bool parse_frm_file(FakeTHD *fake_thd, const char *frm_path)
{
  printf("DEBUG: Entering parse_frm_file\n");
  fflush(stdout);
  
  TABLE_LIST table_list;
  uchar *frm_data= NULL;
  size_t frm_length= 0;
  TABLE_SHARE *share= NULL;
  TABLE *table= NULL;
  LEX_CSTRING db_name, table_name;
  bool error= true;
  
  memset(&table_list, 0, sizeof(table_list));
  printf("DEBUG: table_list initialized\n");
  fflush(stdout);

  printf("DEBUG: About to read FRM file: %s\n", frm_path);
  fflush(stdout);
  
  frm_data= read_frm_file(frm_path, &frm_length);
  if (!frm_data)
    goto cleanup;
  
  printf("DEBUG: FRM file read successfully, size: %zu bytes\n", frm_length);
  fflush(stdout);

  printf("DEBUG: Extracting database and table names\n");
  fflush(stdout);
  
  if (extract_db_table_names(frm_path, &db_name, &table_name))
  {
    fprintf(stderr, "Error: Cannot extract database and table names from path\n");
    goto cleanup;
  }
  
  printf("DEBUG: Names extracted - db: %.*s, table: %.*s\n", 
         (int)db_name.length, db_name.str, (int)table_name.length, table_name.str);
  fflush(stdout);

  printf("DEBUG: Allocating TABLE_SHARE manually\n");
  fflush(stdout);
  
  share = (TABLE_SHARE*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE_SHARE), MYF(MY_ZEROFILL));
  if (!share)
  {
    fprintf(stderr, "Error: Cannot allocate TABLE_SHARE\n");
    goto cleanup;
  }
  
  share->db.str = db_name.str;
  share->db.length = db_name.length;
  share->table_name.str = table_name.str;
  share->table_name.length = table_name.length;
  
  init_alloc_root(PSI_NOT_INSTRUMENTED, &share->mem_root, 1024, 0, MYF(0));
  
  fake_thd->mem_root = share->mem_root;
  
  printf("DEBUG: TABLE_SHARE allocated and initialized successfully\n");
  fflush(stdout);
  
  printf("DEBUG: About to call init_from_binary_frm_image\n");
  fflush(stdout);
  
  if (share->init_from_binary_frm_image((THD*)fake_thd, false, frm_data, frm_length))
  {
    fprintf(stderr, "Error: Cannot parse FRM file - init_from_binary_frm_image failed\n");
    goto cleanup;
  }
  
  printf("DEBUG: init_from_binary_frm_image completed successfully\n");
  fflush(stdout);
  

  table= static_cast<TABLE *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE), MYF(MY_ZEROFILL)));
  if (!table)
  {
    fprintf(stderr, "Error: Cannot allocate TABLE structure\n");
    goto cleanup;
  }
  
  table->s= share;
  table->in_use= (THD*)fake_thd;
  
  table_list.table_name= Lex_ident_table(table_name);
  table_list.db= Lex_ident_db(db_name);
  table_list.alias= Lex_ident_table(table_name);
  
  table->init((THD*)fake_thd, &table_list);
  
  printf("Table: %.*s.%.*s\n", (int)db_name.length, db_name.str, (int)table_name.length, table_name.str);
  printf("Fields: %d\n", share->fields);
  printf("Keys: %d\n", share->keys);
  
  error= false;
  
cleanup:
  if (share)
  {
    free_root(&share->mem_root, MYF(0));
    my_free(share);
  }
  my_free(table);
  my_free(frm_data);
  my_free((void*)db_name.str);
  my_free((void*)table_name.str);
  return error;
}

/**
  Main function
*/
int main(int argc, char **argv)
{
  printf("DEBUG: Starting frm_parser...\n");
  fflush(stdout);
  
  MY_INIT(argv[0]);
  printf("DEBUG: MY_INIT completed\n");
  fflush(stdout);
  
  FakeTHD *fake_thd= NULL;
  int exit_code= 0;

  if (argc < 2)
  {
    fprintf(stderr, "Usage: %s <frm_file>\n", argv[0]);
    return 1;
  }

  printf("DEBUG: Arguments validated, FRM file: %s\n", argv[1]);
  fflush(stdout);

  printf("DEBUG: About to initialize THD...\n");
  fflush(stdout);

  my_thread_init();
  my_mutex_init();
  fake_thd= init_fake_thd();
  if (!fake_thd)
  {
    fprintf(stderr, "Error: Cannot initialize THD\n");
    exit_code= 1;
    goto exit;
  }
  
  printf("DEBUG: THD initialized successfully, about to parse FRM file...\n");
  fflush(stdout);

  if (parse_frm_file(fake_thd, argv[1]))
  {
    exit_code= 1;
  }
  
  printf("DEBUG: FRM parsing completed\n");
  fflush(stdout);

  exit:
    cleanup_fake_thd(fake_thd);
  my_thread_end();
  my_mutex_end();
  my_end(0);
  return exit_code;
}
