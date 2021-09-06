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

#if defined(MYSQL_SERVER) && !defined MYSQL_SERVICE_SQL
#define MYSQL_SERVICE_SQL

#include <mysql.h>

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
  MYSQL *(STDCALL *mysql_init)(MYSQL *mysql);
  MYSQL *(*mysql_real_connect_local)(MYSQL *mysql,
    const char *host, const char *user, const char *db,
    unsigned long clientflag);
  MYSQL *(STDCALL *mysql_real_connect)(MYSQL *mysql, const char *host,
      const char *user, const char *passwd, const char *db, unsigned int port,
      const char *unix_socket, unsigned long clientflag);
  unsigned int(STDCALL *mysql_errno)(MYSQL *mysql);
  const char *(STDCALL *mysql_error)(MYSQL *mysql);
  int (STDCALL *mysql_real_query)(MYSQL *mysql, const char *q,
                                  unsigned long length);
  my_ulonglong (STDCALL *mysql_affected_rows)(MYSQL *mysql);
  my_ulonglong (STDCALL *mysql_num_rows)(MYSQL_RES *res);
  MYSQL_RES *(STDCALL *mysql_store_result)(MYSQL *mysql);
  void (STDCALL *mysql_free_result)(MYSQL_RES *result);
  MYSQL_ROW (STDCALL *mysql_fetch_row)(MYSQL_RES *result);
  void (STDCALL *mysql_close)(MYSQL *sock);
} *sql_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define mysql_init sql_service->mysql_init
#define mysql_real_connect_local sql_service->mysql_real_connect_local
#define mysql_real_connect sql_service->mysql_real_connect
#define mysql_errno(M) sql_service->mysql_errno(M)
#define mysql_error(M) sql_service->mysql_error(M)
#define mysql_real_query sql_service->mysql_real_query
#define mysql_affected_rows sql_service->mysql_affected_rows
#define mysql_num_rows sql_service->mysql_num_rows
#define mysql_store_result sql_service->mysql_store_result
#define mysql_free_result sql_service->mysql_free_result
#define mysql_fetch_row sql_service->mysql_fetch_row
#define mysql_close sql_service->mysql_close

#else

MYSQL *mysql_real_connect_local(MYSQL *mysql,
    const char *host, const char *user, const char *db,
    unsigned long clientflag);

/* The rest of the function declarations mest be taken from the mysql.h */

#endif /*MYSQL_DYNAMIC_PLUGIN*/


#ifdef __cplusplus
}
#endif

#endif /*MYSQL_SERVICE_SQL */


