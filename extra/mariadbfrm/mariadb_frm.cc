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
#include "sql_plugin.h"
#include "mysqld.h"
#include <memory>
#include <functional>

static bool debug_enabled= false;

#define DEBUG(fmt, ...)                                                       \
  do                                                                          \
  {                                                                           \
    if (debug_enabled)                                                        \
    {                                                                         \
      fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__);                          \
      fflush(stderr);                                                         \
    }                                                                         \
  } while (0)

extern mysql_mutex_t LOCK_thread_id;
extern handlerton *get_frm_mock_handlerton();
extern PSI_mutex_key key_LOCK_thread_id;
extern mysql_mutex_t LOCK_plugin;
extern bool plugins_are_initialized;
extern st_plugin_int **plugin_array;
extern uint plugin_array_size;

PSI_mutex_key key_LOCK_plugin;

char server_uuid[37]= "12345678-1234-1234-1234-123456789012";
ulong thread_id= 1;

static st_maria_plugin mock_plugin= {
    .type= MYSQL_STORAGE_ENGINE_PLUGIN,
    .info= nullptr,
    .name= "MOCK_ENGINE",
    .author= "hp77",
    .descr= "Mock storage engine for FRM parsing",
    .license= PLUGIN_LICENSE_GPL,
    .version= 0x0100,
    .version_info= "1.0",
    .maturity= MariaDB_PLUGIN_MATURITY_STABLE};

static st_plugin_int mock_plugin_int= {
    .name= {const_cast<char *>("mock_storage_engine"), 19},
    .plugin= &mock_plugin,
    .plugin_dl= nullptr,
    .ptr_backup= nullptr,
    .nbackups= 0,
    .state= PLUGIN_IS_READY,
    .ref_count= 1,
    .locks_total= 0,
    .data= nullptr,
    .mem_root= {},
    .system_vars= nullptr,
    .load_option= PLUGIN_ON};

static st_plugin_int *mock_plugin_ptr= &mock_plugin_int;
static plugin_ref mock_plugin_ref= plugin_int_to_ref(mock_plugin_ptr);

static int init_thread_environment()
{
  mysql_mutex_init(key_LOCK_start_thread, &LOCK_start_thread,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_status, &LOCK_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_system_variables,
                   &LOCK_global_system_variables, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_user_conn, &LOCK_user_conn, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thread_id, &LOCK_thread_id, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_start_thread, &COND_start_thread, nullptr);

  return 0;
}

static int mysql_init_variables()
{
  auto &gv= global_system_variables;
  gv.character_set_client= &my_charset_utf8mb3_general_ci;
  gv.collation_connection= &my_charset_utf8mb3_general_ci;
  gv.collation_database= &my_charset_utf8mb3_general_ci;
  gv.character_set_results= &my_charset_utf8mb3_general_ci;
  gv.character_set_filesystem= &my_charset_bin;
  gv.table_plugin= mock_plugin_ref;
  gv.tmp_table_plugin= mock_plugin_ref;
  return 0;
}

static int init_early_variables()
{
  sf_leaking_memory= 1;
  mysqld_server_started= mysqld_server_initialized= 0;
  default_charset_info= &my_charset_utf8mb3_general_ci;
  return 0;
}

static int init_character_sets()
{
  system_charset_info= &my_charset_utf8mb3_general_ci;
  files_charset_info= &my_charset_utf8mb3_general_ci;
  national_charset_info= &my_charset_utf8mb3_general_ci;
  table_alias_charset= &my_charset_bin;
  character_set_filesystem= &my_charset_bin;
  default_charset_info= &my_charset_utf8mb3_general_ci;
  return 0;
}

static int init_plugin_system_complete()
{
  mysql_mutex_init(key_LOCK_plugin, &LOCK_plugin, MY_MUTEX_INIT_SLOW);
  my_rnd_init(&sql_rand, static_cast<ulong>(server_start_time),
              static_cast<ulong>(server_start_time) / 2);
  handlerton *actual_hton= get_frm_mock_handlerton();

  mock_plugin.info= actual_hton;
  mock_plugin_int.data= actual_hton;

  actual_hton->flags= HTON_CAN_RECREATE;
  actual_hton->db_type= DB_TYPE_BLACKHOLE_DB;
  actual_hton->slot= 0;
  hton2plugin[0]= &mock_plugin_int;
  plugins_are_initialized= true;

  return 0;
}
/**
  Initialize THD structure
*/
static THD *create_minimal_thd()
{
  THD *thd= new THD(1, false);

  char stack_dummy;
  thd->thread_stack= &stack_dummy;
  thd->set_psi(nullptr);

  thd->push_internal_handler(new Turn_errors_to_warnings_handler());
  LEX *lex= new (thd->mem_root) LEX();
  thd->lex= lex;
  lex_start(thd);
  thd->variables.sql_mode= 0;

  thd->variables.sql_mode|= MODE_NO_ENGINE_SUBSTITUTION;
  thd->variables.old_behavior= 0;
  thd->variables.collation_server= default_charset_info;

  strncpy(thd->security_ctx->priv_user, "root",
          sizeof(thd->security_ctx->priv_user) - 1);
  thd->security_ctx->priv_user[sizeof(thd->security_ctx->priv_user) - 1]= '\0';
  strncpy(thd->security_ctx->priv_host, "localhost",
          sizeof(thd->security_ctx->priv_host) - 1);
  thd->security_ctx->priv_host[sizeof(thd->security_ctx->priv_host) - 1]= '\0';
  thd->security_ctx->host_or_ip= "localhost";

  thd->stmt_arena= thd;
  thd->set_stmt_da(new Diagnostics_area(false));
  // for geometric types
  bzero(&thd->status_var, sizeof(thd->status_var));
  bzero(&thd->org_status_var, sizeof(thd->org_status_var));
  thd->status_var.flush_status_time= my_time(0);

  lex->sql_command= SQLCOM_SHOW_CREATE;
  lex->create_info.init();

  return thd;
}

static int init_sql_functions()
{
  if (item_create_init())
    return 1;
  return 0;
}

static void cleanup_sql_functions() { item_create_cleanup(); }

/**
  Read FRM file into memory
*/
static uchar *read_frm_file(const char *filename, size_t *length)
{
  File file;
  MY_STAT stat_info{};
  uchar *buffer= nullptr;

  if (my_stat(filename, &stat_info, MYF(0)) == nullptr)
  {
    DEBUG("Error: Cannot stat file '%s': %s\n", filename, strerror(errno));
    return nullptr;
  }

  *length= stat_info.st_size;

  if (!(buffer= static_cast<uchar *>(
            my_malloc(PSI_NOT_INSTRUMENTED, *length, MYF(MY_WME)))))
  {
    DEBUG("Error: Cannot allocate memory for FRM file\n");
    return nullptr;
  }

  if ((file= mysql_file_open(key_file_frm, filename, O_RDONLY | O_SHARE,
                             MYF(0))) < 0)
  {
    DEBUG("Error: Cannot open file '%s': %s\n", filename, strerror(errno));
    my_free(buffer);
    return nullptr;
  }

  if (mysql_file_read(file, buffer, *length, MYF(MY_NABP)))
  {
    DEBUG("Error: Cannot read file '%s': %s\n", filename, strerror(errno));
    my_free(buffer);
    mysql_file_close(file, MYF(0));
    return nullptr;
  }

  mysql_file_close(file, MYF(0));
  return buffer;
}

/**
  Map legacy DB type to engine name
*/
static const char *
get_engine_name_from_legacy_type(enum legacy_db_type db_type)
{
  switch (db_type)
  {
  case DB_TYPE_MYISAM:
    return "MyISAM";
  case DB_TYPE_INNODB:
    return "InnoDB";
  case DB_TYPE_ARIA:
    return "Aria";
  case DB_TYPE_ARCHIVE_DB:
    return "ARCHIVE";
  case DB_TYPE_CSV_DB:
    return "CSV";
  case DB_TYPE_HEAP:
    return "MEMORY";
  case DB_TYPE_BLACKHOLE_DB:
    return "BLACKHOLE";
  case DB_TYPE_FEDERATED_DB:
    return "FEDERATED";
  case DB_TYPE_MRG_MYISAM:
    return "MRG_MyISAM";
  case DB_TYPE_PARTITION_DB:
    return "partition";
  case DB_TYPE_SEQUENCE:
    return "SEQUENCE";
  case DB_TYPE_S3:
    return "S3";
  default:
    return "UNKNOWN";
  }
}

/**
  Extract database and table name from FRM file path
*/
static bool extract_db_table_names(const char *frm_path, LEX_CSTRING *db_name,
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
static int parse_frm_file(THD *fake_thd, const char *frm_path)
{
  DEBUG("Entering parse_frm_file\n");

  // Stack-allocated structures
  TABLE_LIST table_list;
  TABLE_SHARE share{};
  TABLE table{};
  TDC_element tdc{.ref_count= 1};
  size_t frm_length= 0;
  uchar *frm_data_raw;
  LEX_CSTRING db_name{}, table_name{};
  int open_result, show_result;
  legacy_db_type real_engine_type;

  DEBUG("table_list initialized\n");
  DEBUG("About to read FRM file: %s\n", frm_path);

  // Read FRM file with smart pointer
  frm_data_raw= read_frm_file(frm_path, &frm_length);
  if (!frm_data_raw)
  {
    DEBUG("Failed to read FRM file\n");
    return 1;
  }

  // Use unique_ptr with my_free directly as deleter
  std::unique_ptr<uchar, decltype(&my_free)> frm_data(frm_data_raw, my_free);

  if (extract_db_table_names(frm_path, &db_name, &table_name))
  {
    DEBUG("Error: Cannot extract database and table names from path\n");
    return 1;
  }

  std::unique_ptr<char, decltype(&my_free)> db_name_ptr(
      const_cast<char *>(db_name.str), my_free);
  std::unique_ptr<char, decltype(&my_free)> table_name_ptr(
      const_cast<char *>(table_name.str), my_free);
  table_list.init_one_table(&db_name, &table_name, &table_name, TL_READ);

  DEBUG("Names extracted - db: %.*s, table: %.*s\n",
        static_cast<int>(db_name.length), db_name.str, (int) table_name.length,
        table_name.str);

  DEBUG("Initializing stack-allocated TABLE_SHARE\n");

  mysql_mutex_init(0, &share.LOCK_share, MY_MUTEX_INIT_FAST);

  share.db.str= db_name.str;
  share.db.length= db_name.length;
  share.table_name.str= table_name.str;
  share.table_name.length= table_name.length;
  share.tdc= &tdc;

  init_alloc_root(PSI_NOT_INSTRUMENTED, &share.mem_root, 1024, 0, MYF(0));

  fake_thd->lex->sql_command= SQLCOM_SHOW_CREATE;
  strncpy(fake_thd->security_ctx->priv_user, "root",
          sizeof(fake_thd->security_ctx->priv_user) - 1);
  fake_thd->security_ctx
      ->priv_user[sizeof(fake_thd->security_ctx->priv_user) - 1]= '\0';

  DEBUG("About to call init_from_binary_frm_image\n");
  size_t key_length= db_name.length + 1 + table_name.length + 1;
  if (auto key_buff=
          static_cast<char *>(alloc_root(&share.mem_root, key_length)))
  {
    memcpy(key_buff, db_name.str, db_name.length);
    key_buff[db_name.length]= '\0';
    memcpy(key_buff + db_name.length + 1, table_name.str, table_name.length);
    key_buff[db_name.length + 1 + table_name.length]= '\0';

    share.table_cache_key.str= key_buff;
    share.table_cache_key.length= key_length;
  }
  char path_buff[512];
  snprintf(path_buff, sizeof(path_buff), "%s/%s", db_name.str, table_name.str);
  if (auto norm_path= static_cast<char *>(
          alloc_root(&share.mem_root, strlen(path_buff) + 1)))
  {
    strcpy(norm_path, path_buff);
    share.normalized_path.str= norm_path;
    share.normalized_path.length= strlen(norm_path);
  }

  share.path= share.normalized_path;
  share.table_category= TABLE_CATEGORY_USER;
  share.tmp_table= NO_TMP_TABLE;
  share.db_plugin= mock_plugin_ref;

  share.field= nullptr;
  share.fields= 0;
  THD *saved_current_thd= current_thd;
  set_current_thd(fake_thd);

  auto cleanup_mem_root= [&]() { free_root(&share.mem_root, MYF(0)); };
  std::unique_ptr<void, std::function<void(void *)>> mem_root_cleanup(
      &share.mem_root, [&](void *) { cleanup_mem_root(); });

  int parse_error= share.init_from_binary_frm_image(
      fake_thd, false, frm_data.get(), frm_length, nullptr, 0, true);
  set_current_thd(saved_current_thd);

  if (parse_error != 0)
  {
    DEBUG("Error: Cannot parse FRM file - init_from_binary_frm_image failed "
          "with error %d: %s\n",
          my_errno, strerror(my_errno));
    return 1;
  }

  DEBUG("init_from_binary_frm_image completed successfully\n");

  set_current_thd(fake_thd);
  open_result=
      open_table_from_share(fake_thd, &share, &table_name, HA_OPEN_KEYFILE,
                            EXTRA_RECORD, 0, &table, false, nullptr, true);
  if (open_result)
  {
    DEBUG("Error: open_table_from_share failed with error %d\n", open_result);
    return open_result;
  }

  table.s= &share;
  table.in_use= fake_thd;

  table_list.table_name= Lex_ident_table(table_name);
  table_list.db= Lex_ident_db(db_name);
  table_list.alias= Lex_ident_table(table_name);
  table_list.table= &table;

  if (!table.file)
  {
    handlerton *hton= get_frm_mock_handlerton();
    if (hton && hton->create)
    {
      table.file= hton->create(hton, &share, &table.mem_root);
      if (table.file)
      {
        table.file->init();
      }
    }
  }

  String ddl_buffer;
  show_result= show_create_table(fake_thd, &table_list, &ddl_buffer, nullptr,
                                 WITHOUT_DB_NAME);
  if (show_result)
  {
    DEBUG("Error: open_table_from_share failed with error %d\n", show_result);
    return show_result;
  }
  real_engine_type= static_cast<enum legacy_db_type>((uint) frm_data.get()[3]);
  const char *real_engine_name=
      get_engine_name_from_legacy_type(real_engine_type);

  const char *original_ddl= ddl_buffer.c_ptr();
  if (const char *mock_pos= strstr(original_ddl, "mock_storage_engine"))
  {
    String corrected_ddl;
    corrected_ddl.append(original_ddl, mock_pos - original_ddl);
    corrected_ddl.append(real_engine_name, strlen(real_engine_name));
    const char *remainder= mock_pos + strlen("mock_storage_engine");
    corrected_ddl.append(remainder, strlen(remainder));
    printf("%s\n", corrected_ddl.c_ptr());
  }
  else
  {
    printf("%s\n", original_ddl);
  }

  set_current_thd(saved_current_thd);
  return 0;
}

void cleanup()
{
  delete type_handler_data;
  type_handler_data= nullptr;
  cleanup_sql_functions();
  my_thread_end();
  mysql_mutex_destroy(&LOCK_start_thread);
  mysql_mutex_destroy(&LOCK_status);
  mysql_mutex_destroy(&LOCK_global_system_variables);
  mysql_mutex_destroy(&LOCK_user_conn);
  mysql_mutex_destroy(&LOCK_thread_id);
  mysql_cond_destroy(&COND_start_thread);
  my_end(0);
}

/**
  Parse command line arguments
*/
static bool parse_arguments(int argc, char **argv, const char **frm_file)
{
  *frm_file= nullptr;

  for (int i= 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0)
    {
      debug_enabled= true;
    }
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
    {
      printf("Usage: %s [OPTIONS] <frm_file>\n", argv[0]);
      printf("Extract table structure from .frm files\n\n");
      printf("Options:\n");
      printf("  -d, --debug    Enable debug output\n");
      printf("  -h, --help     Show this help message\n\n");
      printf("Example:\n");
      printf("  %s table.frm\n", argv[0]);
      printf("  %s --debug table.frm\n", argv[0]);
      return false;
    }
    else if (argv[i][0] == '-')
    {
      fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
      fprintf(stderr, "Use --help for usage information\n");
      return false;
    }
    else
    {
      if (*frm_file != nullptr)
      {
        fprintf(stderr, "Error: Multiple FRM files specified\n");
        return false;
      }
      *frm_file= argv[i];
    }
  }

  if (*frm_file == nullptr)
  {
    fprintf(stderr, "Error: No FRM file specified\n");
    fprintf(stderr, "Usage: %s [OPTIONS] <frm_file>\n", argv[0]);
    fprintf(stderr, "Use --help for more information\n");
    return false;
  }

  return true;
}

/**
  Main function
*/
int main(int argc, char **argv)
{
  int exit_code{};
  const char *frm_file;

  if (!parse_arguments(argc, argv, &frm_file))
  {
    return 1;
  }

  DEBUG("Starting frm_parser...\n");
  MY_INIT(argv[0]);
  DEBUG("MY_INIT completed\n");

  DEBUG("Arguments validated, FRM file: %s\n", frm_file);

  init_character_sets();
  init_thread_environment();
  init_early_variables();
  mysql_init_variables();
  init_sql_functions();

  if (!(type_handler_data= new Type_handler_data) || type_handler_data->init())
  {
    DEBUG("Error: Cannot initialize type handler system\n");
    return 1;
  }
  init_plugin_system_complete();
  if (my_thread_init())
  {
    DEBUG("Error: Cannot initialize required thread subsystems\n");
    return 1;
  }

  auto thd_deleter= [](THD *) {};
  std::unique_ptr<THD, decltype(thd_deleter)> fake_thd(create_minimal_thd(),
                                                       thd_deleter);

  DEBUG("THD initialized successfully, about to parse FRM file...\n");

  exit_code= parse_frm_file(fake_thd.get(), frm_file);

  if (exit_code > 0)
  {
    DEBUG("FRM parsing completed with exit code: %d\n", exit_code);
    cleanup();
    return exit_code;
  }

  return exit_code;
}
