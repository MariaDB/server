/* Copyright (c) 2017, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "clone_handler.h"

#include <assert.h>
#include <string.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "my_global.h"
#include "my_dir.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysql/plugin_clone.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/psi/mysql_thread.h"
#include "mysqld_error.h"
#include "mysqld.h"
#include "sql_class.h"
#include "sql_parse.h"
#include "sql_plugin.h"  // plugin_unlock
#include "sql_string.h"  // to_lex_cstring
#include "sql_table.h"   // filename_to_tablename
#include "violite.h"

class THD;

/** Clone handler global */
Clone_handler *clone_handle= nullptr;

/** Clone plugin name */
const char *clone_plugin_nm= "clone";


int Clone_handler::clone_local(THD *thd, const char *data_dir)
{
  char dir_name[FN_REFLEN];

  int error= validate_dir(data_dir, dir_name);

  if (!error)
    error= m_plugin_handle->clone_local(thd, dir_name);

  return error;
}


int Clone_handler::clone_remote_server(THD *thd, MYSQL_SOCKET socket)
{
  auto err= m_plugin_handle->clone_server(thd, socket);
  return err;
}

int Clone_handler::init()
{
  const char* name= m_plugin_name.c_str();
  size_t name_length= name ? strlen(name) : 0;
  LEX_CSTRING cstr= {name, name_length};

  plugin_ref plugin= my_plugin_lock_by_name(nullptr, &cstr,
                                            MariaDB_CLONE_PLUGIN);
  if (!plugin)
  {
    m_plugin_handle= nullptr;
    const char* mesg= my_get_err_msg(ER_CLONE_PLUGIN_NOT_LOADED_TRACE);
    my_printf_error(ER_CLONE_PLUGIN_NOT_LOADED_TRACE, "%s", ME_ERROR_LOG_ONLY,
                    mesg);
    return 1;
  }

  m_plugin_handle= (Mysql_clone *)plugin_decl(plugin)->info;
  plugin_unlock(nullptr, plugin);

  if (opt_bootstrap)
    /* Inform that database initialization in progress. */
    return ER_SERVER_SHUTDOWN;

  return 0;
}

int Clone_handler::validate_dir(const char *in_dir, char *out_dir)
{
  MY_STAT stat_info;

  /* Verify that it is absolute path. */
  if (!test_if_hard_path(in_dir))
  {
    my_error(ER_WRONG_VALUE, MYF(0), "path", in_dir);
    return ER_WRONG_VALUE;
  }

  /* Verify that the length is not too long. */
  if (strlen(in_dir) >= FN_REFLEN - 1)
  {
    my_error(ER_PATH_LENGTH, MYF(0), "DATA DIRECTORY");
    return ER_PATH_LENGTH;
  }

  /* Convert the path to native os format. */
  convert_dirname(out_dir, in_dir, nullptr);

  /* Check if the data directory exists already. */
  if (mysql_file_stat(key_file_misc, out_dir, &stat_info, MYF(0)))
  {
    my_error(ER_DB_CREATE_EXISTS, MYF(0), in_dir);
    return ER_DB_CREATE_EXISTS;
  }

  /* Check if path is within current data directory */
  char tmp_dir[FN_REFLEN + 1];
  size_t length;

  strncpy(tmp_dir, out_dir, FN_REFLEN);
  length= strlen(out_dir);

  /* Loop and remove all non-existent directories from the tail */
  while (length)
  {
    /* Check if directory exists. */
    if (mysql_file_stat(key_file_misc, tmp_dir, &stat_info, MYF(0)))
    {
      /* Check if the path is not within data directory. */
      if (test_if_data_home_dir(tmp_dir))
      {
        my_error(ER_PATH_IN_DATADIR, MYF(0), in_dir);
        return ER_PATH_IN_DATADIR;
      }
      break;
    }

    size_t new_length;
    tmp_dir[length - 1]= '\0';

    /* Remove the last directory separator from string */
    dirname_part(tmp_dir, tmp_dir, &new_length);

    /* length must always decrease for the loop to terminate */
    if (length <= new_length)
    {
      assert(false);
      break;
    }

    length= new_length;
  }
  return 0;
}

int clone_handle_create(const char *plugin_name)
{
  if (clone_handle)
  {
    const char* mesg= my_get_err_msg(ER_CLONE_HANDLER_EXIST_TRACE);
    my_printf_error(ER_CLONE_HANDLER_EXIST_TRACE, "%s", ME_ERROR_LOG_ONLY,
                    mesg);
    return 1;
  }

  clone_handle= new Clone_handler(plugin_name);

  if (!clone_handle)
  {
    const char* mesg= my_get_err_msg(ER_CLONE_CREATE_HANDLER_FAIL_TRACE);
    my_printf_error(ER_CLONE_CREATE_HANDLER_FAIL_TRACE, "%s",
                    ME_ERROR_LOG_ONLY, mesg);
    return 1;
  }

  return clone_handle->init();
}

int clone_handle_check_drop(MYSQL_PLUGIN plugin_info)
{
  auto plugin= static_cast<st_plugin_int *>(plugin_info);
  int error= 0;

  mysql_mutex_lock(&LOCK_plugin);
  assert(plugin->state == PLUGIN_IS_DYING);

  if (plugin->ref_count > 0)
    error= WARN_PLUGIN_BUSY;

  mysql_mutex_unlock(&LOCK_plugin);
  return error;
}

int clone_handle_drop()
{
  if (!clone_handle)
    return 1;

  delete clone_handle;

  clone_handle= nullptr;

  if (opt_bootstrap)
    /* Inform that database initialization in progress. */
    return ER_SERVER_SHUTDOWN;

  return 0;
}

Clone_handler *clone_plugin_lock(THD *thd, plugin_ref *plugin)
{
  LEX_CSTRING cstr= {clone_plugin_nm, strlen(clone_plugin_nm)};
  *plugin= my_plugin_lock_by_name(thd, &cstr,
                                  MariaDB_CLONE_PLUGIN);
  mysql_mutex_lock(&LOCK_plugin);

  /* Return handler only if the plugin is ready. We might successfully
  lock the plugin when initialization is progress. */
  if (*plugin && plugin_state(*plugin) == PLUGIN_IS_READY)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    assert(clone_handle);
    return clone_handle;
  }
  mysql_mutex_unlock(&LOCK_plugin);
  return nullptr;
}

void clone_plugin_unlock(THD *thd, plugin_ref plugin)
{
  plugin_unlock(thd, plugin);
}

std::atomic<int> Clone_handler::s_provision_in_progress{0};
std::atomic<bool> Clone_handler::s_is_data_dropped{false};

namespace clone_common
{
bool ends_with(const char *str, const char *suffix)
{
  size_t suffix_len= strlen(suffix);
  size_t str_len= strlen(str);

  return (str_len >= suffix_len &&
          strcmp(str + str_len - suffix_len, suffix) == 0);
}

static void parse_db_table_from_file_path(const char *filepath, char *dbname,
                                          char *tablename)
{
  dbname[0]= '\0';
  tablename[0]= '\0';
  const char *dbname_start= nullptr;
  const char *tablename_start= filepath;
  const char *const_ptr;
  while ((const_ptr= strchr(tablename_start, FN_LIBCHAR)) != NULL)
  {
    dbname_start = tablename_start;
    tablename_start = const_ptr + 1;
  }
  if (!dbname_start)
    return;
  size_t dbname_len = tablename_start - dbname_start - 1;
  if (dbname_len >= FN_REFLEN)
    dbname_len = FN_REFLEN-1;

  strmake(dbname, dbname_start, dbname_len);
  strmake(tablename, tablename_start, FN_REFLEN-1);
  char *ptr;
  if ((ptr = strchr(tablename, '.'))) *ptr= '\0';
  if ((ptr = strstr(tablename, "#P#"))) *ptr= '\0';
  if ((ptr = strstr(tablename, "#i#"))) *ptr= '\0';
}

std::tuple<std::string, std::string, std::string>
convert_filepath_to_tablename(const char *filepath)
{
  char db_name_orig[FN_REFLEN];
  char table_name_orig[FN_REFLEN];
  parse_db_table_from_file_path(filepath, db_name_orig, table_name_orig);
  if (!db_name_orig[0] || !table_name_orig[0])
    return std::make_tuple("", "", "");
  char db_name_conv[FN_REFLEN];
  char table_name_conv[FN_REFLEN];
  filename_to_tablename(db_name_orig, db_name_conv, sizeof(db_name_conv));
  filename_to_tablename(
                table_name_orig, table_name_conv, sizeof(table_name_conv));
        if (!db_name_conv[0] || !table_name_conv[0])
                return std::make_tuple("", "", "");
        return std::make_tuple(db_name_conv, table_name_conv,
                std::string(db_name_orig).append("/").append(table_name_orig));
}

bool is_log_table(const char *dbname, const char *tablename)
{
  assert(dbname);
  assert(tablename);

  LEX_CSTRING lex_db;
  LEX_CSTRING lex_table;

  lex_db.str = dbname;
  lex_db.length = strlen(dbname);
  lex_table.str = tablename;
  lex_table.length = strlen(tablename);

  if (!lex_string_eq(&MYSQL_SCHEMA_NAME, &lex_db))
    return false;

  if (lex_string_eq(&GENERAL_LOG_NAME, &lex_table))
    return true;

  if (lex_string_eq(&SLOW_LOG_NAME, &lex_table))
    return true;

  return false;
}

bool is_stats_table(const char *dbname, const char *tablename)
{
  assert(dbname);
  assert(tablename);

  LEX_CSTRING lex_db;
  LEX_CSTRING lex_table;
  lex_db.str = dbname;
  lex_db.length = strlen(dbname);
  lex_table.str = tablename;
  lex_table.length = strlen(tablename);

  if (!lex_string_eq(&MYSQL_SCHEMA_NAME, &lex_db))
    return false;

  CHARSET_INFO *ci= system_charset_info;

  return (lex_table.length > 4 &&
    /* one of mysql.*_stat tables, but not mysql.innodb* tables*/
    ((my_tolower(ci, lex_table.str[lex_table.length-5]) == 's' &&
    my_tolower(ci, lex_table.str[lex_table.length-4]) == 't' &&
    my_tolower(ci, lex_table.str[lex_table.length-3]) == 'a' &&
    my_tolower(ci, lex_table.str[lex_table.length-2]) == 't' &&
    my_tolower(ci, lex_table.str[lex_table.length-1]) == 's') &&
    !(my_tolower(ci, lex_table.str[0]) == 'i' &&
    my_tolower(ci, lex_table.str[1]) == 'n' &&
    my_tolower(ci, lex_table.str[2]) == 'n' &&
    my_tolower(ci, lex_table.str[3]) == 'o')));
}

int foreach_file_in_dir(
    const fsys::path& dir_path,
    const std::function<void(const fsys::path&)>& callback,
    const std::set<std::string>& file_extns,
    const std::set<fsys::file_type>& file_types, int max_depth)
{
  try
  {
    if (!fsys::exists(dir_path) || !fsys::is_directory(dir_path))
    {
      sql_print_error("Error: %s is not a valid directory.", dir_path.c_str());
      return -1;
    }

    auto options= fsys::directory_options::skip_permission_denied;
    for (auto it= fsys::recursive_directory_iterator(dir_path, options);
         it != fsys::recursive_directory_iterator(); ++it)
    {
      auto& entry= *it;
      int depth= it.depth();

      if (max_depth >= 0 && depth > max_depth)
      {
        it.disable_recursion_pending();
        continue;
      }

      fsys::file_type type= entry.status().type();
      fsys::path filePath= entry.path();

      if (!file_types.empty() && file_types.find(type) == file_types.end())
        continue;

      if (!file_extns.empty())
      {
        std::string extension = filePath.extension().string();
        if (file_extns.find(extension) == file_extns.end())
          continue;
      }
      callback(filePath);
    }
  }
  catch (const fsys::filesystem_error& e)
  {
    sql_print_error("File System Error: %s", e.what());
    return -1;
  }
  catch (const std::exception& e)
  {
    sql_print_error("General Error: %s", e.what());
    return -1;
  }
  return 0;
}

static std::vector<uchar> read_frm_image(File file)
{
  std::vector<uchar> frm_image;
  MY_STAT state;

  if (mysql_file_fstat(file, &state, MYF(MY_WME)))
    return frm_image;

  frm_image.resize((size_t)state.st_size, 0);

  if (mysql_file_read(file, frm_image.data(), (size_t)state.st_size,
                      MYF(MY_NABP)))
    frm_image.clear();
  return frm_image;
}

static
std::string get_table_version_from_image(const std::vector<uchar> &frm_image)
{
  DBUG_ASSERT(frm_image.size() >= 64);
  if (!strncmp((char*) frm_image.data(), "TYPE=VIEW\n", 10))
    return {};

  if (!is_binary_frm_header(frm_image.data()))
    return {};

  /* Length of the MariaDB extra2 segment in the form file. */
  uint len= uint2korr(frm_image.data() + 4);
  const uchar *extra2= frm_image.data() + 64;

  if (*extra2 == '/')   // old frm had '/' there
    return {};

  const uchar *e2end= extra2 + len;
  while (extra2 + 3 <= e2end)
  {
    uchar type= *extra2++;
    size_t length= *extra2++;
    if (!length)
    {
      if (extra2 + 2 >= e2end)
        return {};
      length= uint2korr(extra2);
      extra2+= 2;

      if (length < 256)
        return {};
    }
    if (extra2 + length > e2end)
      return {};
    if (type == EXTRA2_TABLEDEF_VERSION)
    {
      char buff[MY_UUID_STRING_LENGTH];
      my_uuid2str(extra2, buff, 1);
      return std::string(buff, buff + MY_UUID_STRING_LENGTH);
    }
    extra2+= length;
  }
  return {};
}

std::string read_table_version_id(File file)
{
  auto frm_image= read_frm_image(file);
  if (frm_image.empty())
    return {};
  return get_table_version_from_image(frm_image);
}
} // namespace clone_common
