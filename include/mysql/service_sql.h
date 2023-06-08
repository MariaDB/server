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

  Interface for plugins to execute SQL queries on the local server.

  Functions of the service are the 'server-limited'  client library:
     mysql_init
     mysql_real_connect_local
     mysql_real_connect
     mysql_errno
     mysql_error
     mysql_real_query
     mysql_affected_rows
     mysql_num_rows
     mysql_store_result
     mysql_free_result
     mysql_fetch_row
     mysql_close
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

#define mysql_init(M) sql_service->mysql_init_func(M)
#define mysql_real_connect_local(M) sql_service->mysql_real_connect_local_func(M)
#define mysql_real_connect(M,H,U,PW,D,P,S,F) sql_service->mysql_real_connect_func(M,H,U,PW,D,P,S,F)
#define mysql_errno(M) sql_service->mysql_errno_func(M)
#define mysql_error(M) sql_service->mysql_error_func(M)
#define mysql_real_query sql_service->mysql_real_query_func
#define mysql_affected_rows(M) sql_service->mysql_affected_rows_func(M)
#define mysql_num_rows(R) sql_service->mysql_num_rows_func(R)
#define mysql_store_result(M) sql_service->mysql_store_result_func(M)
#define mysql_free_result(R) sql_service->mysql_free_result_func(R)
#define mysql_fetch_row(R) sql_service->mysql_fetch_row_func(R)
#define mysql_close(M) sql_service->mysql_close_func(M)
#define mysql_options(M,O,V) sql_service->mysql_options_func(M,O,V)
#define mysql_fetch_lengths(R) sql_service->mysql_fetch_lengths_func(R)
#define mysql_set_character_set(M,C) sql_service->mysql_set_character_set_func(M,C)
#define mysql_num_fields(R) sql_service->mysql_num_fields_func(R)
#define mysql_select_db(M,D) sql_service->mysql_select_db_func(M,D)
#define mysql_use_result(M) sql_service->mysql_use_result_func(M)
#define mysql_fetch_fields(R) sql_service->mysql_fetch_fields_func(R)
#define mysql_real_escape_string(M,T,F,L) sql_service->mysql_real_escape_string_func(M,T,F,L)
#define mysql_ssl_set(M,K,C1,C2,C3,C4) sql_service->mysql_ssl_set_func(M,K,C1,C2,C3,C4)

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

#endif /*MYSQL_SERVICE_SQL */


