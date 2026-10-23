/* Copyright (C) 2021 MariaDB Corporation

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

#ifndef MYSQL_SERVICE_SQL
#define MYSQL_SERVICE_SQL

#ifndef MYSQL_ABI_CHECK
#include <mysql.h>
#endif

/**
  @file
  SQL service
*/

/**
  @defgroup plugin_api_service_sql SQL service
  @ingroup plugin_api_services
  SQL service

  Interface for plugins to execute SQL queries on the local server.

  Functions of the service are the 'server-limited' client library:

  - @ref mysql_init
  - @ref mysql_real_connect_local
  - @ref mysql_real_connect
  - @ref mysql_errno
  - @ref mysql_error
  - @ref mysql_real_query
  - @ref mysql_affected_rows
  - @ref mysql_num_rows
  - @ref mysql_store_result
  - @ref mysql_free_result
  - @ref mysql_fetch_row
  - @ref mysql_close

  @{
*/


#ifdef __cplusplus
extern "C" {
#endif

extern struct sql_service_st {
  MYSQL *(STDCALL *mysql_init_func)(MYSQL *mysql);
  MYSQL *(*mysql_real_connect_local_func)(MYSQL *mysql);
  MYSQL *(STDCALL *mysql_real_connect_func)(MYSQL *mysql, const char *host,
      const char *user, const char *passwd, const char *db, unsigned int port,
      const char *unix_socket, unsigned long clientflag);
  unsigned int(STDCALL *mysql_errno_func)(MYSQL *mysql);
  const char *(STDCALL *mysql_error_func)(MYSQL *mysql);
  int (STDCALL *mysql_real_query_func)(MYSQL *mysql, const char *q,
                                  unsigned long length);
  my_ulonglong (STDCALL *mysql_affected_rows_func)(MYSQL *mysql);
  my_ulonglong (STDCALL *mysql_num_rows_func)(MYSQL_RES *res);
  MYSQL_RES *(STDCALL *mysql_store_result_func)(MYSQL *mysql);
  void (STDCALL *mysql_free_result_func)(MYSQL_RES *result);
  MYSQL_ROW (STDCALL *mysql_fetch_row_func)(MYSQL_RES *result);
  void (STDCALL *mysql_close_func)(MYSQL *mysql);
  int (STDCALL *mysql_options_func)(MYSQL *mysql, enum mysql_option option,
                            const void *arg);
  unsigned long *(STDCALL *mysql_fetch_lengths_func)(MYSQL_RES *res);
  int (STDCALL *mysql_set_character_set_func)(MYSQL *mysql, const char *cs_name);
  unsigned int (STDCALL *mysql_num_fields_func)(MYSQL_RES *res);
  int (STDCALL *mysql_select_db_func)(MYSQL *mysql, const char *db);
  MYSQL_RES *(STDCALL *mysql_use_result_func)(MYSQL *mysql);
  MYSQL_FIELD *(STDCALL *mysql_fetch_fields_func)(MYSQL_RES *res);
  unsigned long (STDCALL *mysql_real_escape_string_func)(MYSQL *mysql, char *to,
                                        const char *from, unsigned long length);
  my_bool (STDCALL *mysql_ssl_set_func)(MYSQL *mysql, const char *key,
      const char *cert, const char *ca, const char *capath, const char *cipher);
} *sql_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define mysql_init(mysql) sql_service->mysql_init_func(mysql)
#define mysql_real_connect_local(mysql) sql_service->mysql_real_connect_local_func(mysql)
#define mysql_real_connect(mysql,host,user,password,db,port,socket,flags) \
  sql_service->mysql_real_connect_func(mysql,host,user,password,db,port,socket,flags)
#define mysql_errno(mysql) sql_service->mysql_errno_func(mysql)
#define mysql_error(mysql) sql_service->mysql_error_func(mysql)
#define mysql_real_query sql_service->mysql_real_query_func
#define mysql_affected_rows(mysql) sql_service->mysql_affected_rows_func(mysql)
#define mysql_num_rows(result) sql_service->mysql_num_rows_func(result)
#define mysql_store_result(mysql) sql_service->mysql_store_result_func(mysql)
#define mysql_free_result(result) sql_service->mysql_free_result_func(result)
#define mysql_fetch_row(result) sql_service->mysql_fetch_row_func(result)
#define mysql_close(mysql) sql_service->mysql_close_func(mysql)
#define mysql_options(mysql,option,arg) sql_service->mysql_options_func(mysql,option,arg)
#define mysql_fetch_lengths(result) sql_service->mysql_fetch_lengths_func(result)
#define mysql_set_character_set(mysql,cs_name) sql_service->mysql_set_character_set_func(mysql,cs_name)
#define mysql_num_fields(result) sql_service->mysql_num_fields_func(result)
#define mysql_select_db(mysql,db) sql_service->mysql_select_db_func(mysql,db)
#define mysql_use_result(mysql) sql_service->mysql_use_result_func(mysql)
#define mysql_fetch_fields(result) sql_service->mysql_fetch_fields_func(result)
#define mysql_real_escape_string(mysql,to,from,length) sql_service->mysql_real_escape_string_func(mysql,to,from,length)
#define mysql_ssl_set(mysql,key,cert,ca,capath,cipher) sql_service->mysql_ssl_set_func(mysql,key,cert,ca,capath,cipher)

#else

/*
  Establishes the connection to the 'local' server that started the plugin.
  Like the mysql_real_connect() does for the remote server.
  The established connection has no user/host associated to it,
  neither it has the current db, so the queries should have
  database/table name specified.
*/
MYSQL *mysql_real_connect_local(MYSQL *mysql);

/* The rest of the function declarations must be taken from the mysql.h */

#endif /*MYSQL_DYNAMIC_PLUGIN*/


#ifdef __cplusplus
}
#endif

/** @} */

#endif /*MYSQL_SERVICE_SQL */
