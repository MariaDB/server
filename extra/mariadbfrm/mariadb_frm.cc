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

#include "my_sys.h"
#include "mysql_com.h"
#include "m_ctype.h"
#include "field.h"
#include "sql_class.h"
#include "sql_show.h"
#include "table_cache.h"
#include "sql_table.h"

#define DEBUG(fmt, ...) do { fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__); fflush(stderr); } while(0)



extern mysql_mutex_t LOCK_start_thread, LOCK_status, LOCK_global_system_variables, LOCK_user_conn, LOCK_thread_id;
extern mysql_cond_t COND_start_thread;
extern struct system_variables global_system_variables;


PSI_mutex_key key_LOCK_start_thread = 0;
PSI_mutex_key key_LOCK_status = 0;
PSI_mutex_key key_LOCK_global_system_variables = 0;
PSI_mutex_key key_LOCK_user_conn = 0;
PSI_mutex_key key_LOCK_thread_id = 0;
PSI_cond_key key_COND_start_thread = 0;
PSI_mutex_key key_LOCK_plugin = 0;


bool plugins_are_initialized = false;
mysql_mutex_t LOCK_plugin;
struct st_plugin_int **plugin_array = nullptr;
uint plugin_array_size = 0;

ulong server_id = 1;
char server_uuid[MY_UUID_SIZE+1];
ulong binlog_cache_use = 0;
ulong binlog_cache_disk_use = 0;
ulong aborted_threads = 0;
ulong aborted_connects = 0;
unsigned long opt_readonly = 0;
unsigned long long test_flags = 0;
ulong thread_id = 1;


CHARSET_INFO *system_charset_info = nullptr;
CHARSET_INFO *files_charset_info = nullptr;
CHARSET_INFO *national_charset_info = nullptr;
CHARSET_INFO *table_alias_charset = nullptr;
CHARSET_INFO *character_set_filesystem = nullptr;
CHARSET_INFO *default_charset_info = nullptr;

extern int mysqld_server_started;
extern int mysqld_server_initialized;
extern int sf_leaking_memory;
extern handlerton* get_frm_mock_handlerton();
st_plugin_int *hton2plugin[MAX_HA];




static st_maria_plugin mock_plugin = {
  .type = MYSQL_STORAGE_ENGINE_PLUGIN,
  .info = nullptr,
  .name = "MOCK_ENGINE",
  .author = "Nikita Malyavin",
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
  .data= nullptr, 
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

static st_plugin_int *mock_plugin_ptr= &mock_plugin_int;
static plugin_ref mock_plugin_ref= &mock_plugin_ptr;


extern mysql_mutex_t LOCK_plugin;


static int init_thread_environment()
{
  mysql_mutex_init(key_LOCK_start_thread, &LOCK_start_thread, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_status, &LOCK_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_system_variables, &LOCK_global_system_variables, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_user_conn, &LOCK_user_conn, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thread_id, &LOCK_thread_id, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_start_thread, &COND_start_thread, NULL);

  return 0;
}

static int mysql_init_variables()
{
  global_system_variables.character_set_client = &my_charset_utf8mb3_general_ci;
  global_system_variables.collation_connection = &my_charset_utf8mb3_general_ci;
  global_system_variables.collation_database = &my_charset_utf8mb3_general_ci;
  global_system_variables.character_set_results = &my_charset_utf8mb3_general_ci;
  global_system_variables.character_set_filesystem = &my_charset_bin;
  global_system_variables.table_plugin = mock_plugin_ref;
  global_system_variables.tmp_table_plugin = mock_plugin_ref;

  return 0;
}

static int init_early_variables()
{
  sf_leaking_memory = 1;
  mysqld_server_started = mysqld_server_initialized = 0;

  strcpy(server_uuid, "12345678-1234-1234-1234-123456789012");

  return 0;
}

static int init_character_sets()
{
  system_charset_info = &my_charset_utf8mb3_general_ci;
  files_charset_info = &my_charset_utf8mb3_general_ci;
  national_charset_info = &my_charset_utf8mb3_general_ci;
  table_alias_charset = &my_charset_bin;
  character_set_filesystem = &my_charset_bin;
  default_charset_info = &my_charset_utf8mb3_general_ci;
  
  return 0;
}

static int init_plugin_system_complete()
{
  mysql_mutex_init(key_LOCK_plugin, &LOCK_plugin, MY_MUTEX_INIT_SLOW);
  my_rnd_init(&sql_rand,(ulong) server_start_time,(ulong) server_start_time/2);

  plugin_array_size = 16;
  plugin_array = static_cast<struct st_plugin_int **>(my_malloc(
      PSI_NOT_INSTRUMENTED, plugin_array_size * sizeof(struct st_plugin_int *),
      MYF(MY_WME | MY_ZEROFILL)));

  handlerton* actual_hton = get_frm_mock_handlerton();
  
  mock_plugin.info = actual_hton;
  
  mock_plugin_int.data = actual_hton;
  
  actual_hton->flags = HTON_CAN_RECREATE;
  actual_hton->db_type = DB_TYPE_BLACKHOLE_DB;
  actual_hton->slot = 0;
  hton2plugin[0] = &mock_plugin_int;
  if (!plugin_array)
    return 1;

    hton2plugin[0] = &mock_plugin_int;
    
    
  plugins_are_initialized = true;

  return 0;
}
/**
  Initialize fake THD structure
*/
static THD* create_minimal_thd() {
    THD *thd = new THD(1, false);
    
    char stack_dummy;
    thd->thread_stack = &stack_dummy;
    thd->variables.sql_mode = 0;
    
    return thd;
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
    DEBUG("Error: Cannot stat file '%s': %s\n", 
            filename, strerror(errno));
    return NULL;
  }
  
  *length= stat_info.st_size;
  
  if (!(buffer= (uchar*)my_malloc(PSI_NOT_INSTRUMENTED, *length, MYF(MY_WME))))
  {
    DEBUG("Error: Cannot allocate memory for FRM file\n");
    return NULL;
  }
  
  if ((file= mysql_file_open(key_file_frm, filename, O_RDONLY | O_SHARE, MYF(0))) < 0)
  {
    DEBUG("Error: Cannot open file '%s': %s\n", 
            filename, strerror(errno));
    my_free(buffer);
    return NULL;
  }
  
  if (mysql_file_read(file, buffer, *length, MYF(MY_NABP)))
  {
    DEBUG("Error: Cannot read file '%s': %s\n", 
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
static bool parse_frm_file(THD *fake_thd, const char *frm_path)
{
  DEBUG("DEBUG: Entering parse_frm_file\n");
  
  TABLE_LIST table_list;
  uchar *frm_data= NULL;
  size_t frm_length= 0;
  TABLE_SHARE *share= NULL;
  TABLE *table= NULL;
  LEX_CSTRING db_name{}, table_name{};
  bool error= true;
  TDC_element tdc {.ref_count = 1};
  
  memset(&table_list, 0, sizeof(table_list));
  DEBUG("DEBUG: table_list initialized\n");

  DEBUG("DEBUG: About to read FRM file: %s\n", frm_path);
  
  frm_data= read_frm_file(frm_path, &frm_length);

  if (!frm_data)
    return 1;
  if (extract_db_table_names(frm_path, &db_name, &table_name))
  {
    DEBUG("Error: Cannot extract database and table names from path\n");
    return 1;
  }
  
  DEBUG("DEBUG: Names extracted - db: %.*s, table: %.*s\n", 
         (int)db_name.length, db_name.str, (int)table_name.length, table_name.str);

  DEBUG("DEBUG: Allocating TABLE_SHARE manually\n");

  share = (TABLE_SHARE*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE_SHARE), MYF(MY_ZEROFILL));
  if (!share)
  {
    DEBUG("Error: Cannot allocate TABLE_SHARE\n");
    return 1;
  }

mysql_mutex_init(0, &share->LOCK_share, MY_MUTEX_INIT_FAST);

  
  share->db.str = db_name.str;
  share->db.length = db_name.length;
  share->table_name.str = table_name.str;
  share->table_name.length = table_name.length;
  share->tdc = &tdc;
  
  init_alloc_root(PSI_NOT_INSTRUMENTED, &share->mem_root, 1024, 0, MYF(0));
  
  fake_thd->mem_root = &share->mem_root;
  
  DEBUG("DEBUG: TABLE_SHARE allocated and initialized successfully\n");
  
  DEBUG("DEBUG: About to call init_from_binary_frm_image\n");
  size_t key_length = db_name.length + 1 + table_name.length + 1;
  char *key_buff = (char*)alloc_root(&share->mem_root, key_length);
  if (key_buff) {
    memcpy(key_buff, db_name.str, db_name.length);
    key_buff[db_name.length] = '\0';
    memcpy(key_buff + db_name.length + 1, table_name.str, table_name.length);
    key_buff[db_name.length + 1 + table_name.length] = '\0';

    share->table_cache_key.str = key_buff;
    share->table_cache_key.length = key_length;
  }
  char path_buff[512];
  snprintf(path_buff, sizeof(path_buff), "%s/%s", db_name.str, table_name.str);
  char *norm_path = (char*)alloc_root(&share->mem_root, strlen(path_buff) + 1);
  if (norm_path) {
    strcpy(norm_path, path_buff);
    share->normalized_path.str = norm_path;
    share->normalized_path.length = strlen(norm_path);
  }

  share->path = share->normalized_path;
  share->table_category = TABLE_CATEGORY_USER;
  share->tmp_table = NO_TMP_TABLE;
  share->db_plugin = mock_plugin_ref;

  share->field= nullptr;
  share->fields= 0;


  int parse_error = share->init_from_binary_frm_image((THD*)fake_thd, false, frm_data, frm_length);
  if (parse_error != 0)
  {
    DEBUG("Error: Cannot parse FRM file - init_from_binary_frm_image failed with error %d: %s\n",my_errno, 
    strerror(my_errno));
  }
  
  DEBUG("DEBUG: init_from_binary_frm_image completed successfully\n");
  

  table= static_cast<TABLE *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE), MYF(MY_ZEROFILL)));
  if (!table)
  {
    DEBUG("Error: Cannot allocate TABLE structure\n");
  }
 open_table_from_share(fake_thd, share, &table_name, HA_OPEN_KEYFILE,
                      EXTRA_RECORD, 0,
                      table, false); // todo error handling
 
  table->s= share;
  table->in_use= (THD*)fake_thd;
  
  table_list.table_name= Lex_ident_table(table_name);
  table_list.db= Lex_ident_db(db_name);
  table_list.alias= Lex_ident_table(table_name);
  table_list.table = table;

  String ddl_buffer;

  printf("====WITHOUT HEADER:\n\n");
  show_create_table(fake_thd, &table_list, &ddl_buffer, NULL, WITHOUT_DB_NAME);
  printf("%s\n", ddl_buffer.c_ptr());

  printf("\n====WITH HEADER:\n\n");
  ddl_buffer.length(0);
  show_create_table(fake_thd, &table_list, &ddl_buffer, NULL, WITH_DB_NAME);
  printf("--\n-- Table structure for table `%s`\n--\n\n%s\n",
         table_name.str, ddl_buffer.c_ptr());
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
  DEBUG("DEBUG: Starting frm_parser...\n");
  
  MY_INIT(argv[0]);
  DEBUG("DEBUG: MY_INIT completed\n");
  
  THD *fake_thd= NULL;
  int exit_code= 0;

  if (argc < 2)
  {
    DEBUG("Usage: %s <frm_file>\n", argv[0]);
    return 1;
  }

  DEBUG("DEBUG: Arguments validated, FRM file: %s\n", argv[1]);

  DEBUG("DEBUG: About to initialize THD...\n");


  init_character_sets();
  init_thread_environment();
  init_early_variables();
  mysql_init_variables();
  if (init_plugin_system_complete())
  {
    DEBUG("Error: Cannot initialize required subsystems\n");
    return 1;
  }

  my_thread_init();
  fake_thd= create_minimal_thd();

  DEBUG("DEBUG: THD initialized successfully, about to parse FRM file...\n");

  if (parse_frm_file(fake_thd, argv[1]))
  {
    exit_code= 1;
  }
  
  DEBUG("DEBUG: FRM parsing completed\n");


  delete fake_thd;
  mysql_mutex_destroy(&LOCK_start_thread);
  mysql_mutex_destroy(&LOCK_status);
  mysql_mutex_destroy(&LOCK_global_system_variables);
  mysql_mutex_destroy(&LOCK_user_conn);
  mysql_mutex_destroy(&LOCK_thread_id);
  mysql_cond_destroy(&COND_start_thread);
  my_thread_end();
  my_end(0);
  return exit_code;
}
