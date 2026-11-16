/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef MYSQL_SERVICE_SQL_EXTENSION_INCLUDED
#define MYSQL_SERVICE_SQL_EXTENSION_INCLUDED

/**
  @file include/mysql/service_sql_extension.h
  SQL extension service: what every MariaDB_SQL_EXTENSION_PLUGIN can use
  in its execute() function. The other SQL extension services are in
  service_sql_extension_*.h.
*/

/**
  @defgroup plugin_api_service_sql_extension SQL Extension Service
  @ingroup plugin_api_services

  The services of the execute() function of an SQL extension plugin
  (include/mysql/plugin_sql_extension.h). Their functions may only be
  called from execute(), on the THD it was given. This one checks the
  client's privileges. The others are for plugins that need them:
  - service_sql_extension_result.h: send result sets and OK packets;
  - service_sql_extension_client.h: run SQL as the client.

  A statement answers with an error raised with my_error() /
  my_printf_error(), or with what the plugin sent through those services.
  When execute() returns 0 without sending anything, the client gets a
  plain OK. Notes and warnings are added to the statement with
  my_printf_error() and ME_NOTE or ME_WARNING.

  Functions returning int return 0 on success and non-zero after raising
  an error, which execute() should then return. Any other invalid call (a
  thd other than the one execute() was given, a NULL handle or query, an
  empty database or table name, privileges of 0 or with unknown bits)
  raises ER_STD_INVALID_ARGUMENT naming the argument, in all builds (not
  only an assertion).
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

/**
  Privileges for sql_extension_check_access(), combined with |: the ones
  GRANT accepts on a database, so every privilege of a database or of a
  table can be checked.
*/
#define SQL_EXTENSION_PRIV_SELECT         (1ULL << 0)
#define SQL_EXTENSION_PRIV_INSERT         (1ULL << 1)
#define SQL_EXTENSION_PRIV_UPDATE         (1ULL << 2)
#define SQL_EXTENSION_PRIV_DELETE         (1ULL << 3)
#define SQL_EXTENSION_PRIV_CREATE         (1ULL << 4)
#define SQL_EXTENSION_PRIV_DROP           (1ULL << 5)
#define SQL_EXTENSION_PRIV_ALTER          (1ULL << 6)
#define SQL_EXTENSION_PRIV_INDEX          (1ULL << 7)
#define SQL_EXTENSION_PRIV_CREATE_VIEW    (1ULL << 8)
#define SQL_EXTENSION_PRIV_SHOW_VIEW      (1ULL << 9)
#define SQL_EXTENSION_PRIV_TRIGGER        (1ULL << 10)
#define SQL_EXTENSION_PRIV_EXECUTE        (1ULL << 11)
#define SQL_EXTENSION_PRIV_CREATE_ROUTINE (1ULL << 12)
#define SQL_EXTENSION_PRIV_ALTER_ROUTINE  (1ULL << 13)
#define SQL_EXTENSION_PRIV_REFERENCES     (1ULL << 14)
#define SQL_EXTENSION_PRIV_GRANT_OPTION   (1ULL << 15)
#define SQL_EXTENSION_PRIV_DELETE_HISTORY (1ULL << 16)
#define SQL_EXTENSION_PRIV_CREATE_TEMPORARY_TABLES (1ULL << 17)
#define SQL_EXTENSION_PRIV_LOCK_TABLES    (1ULL << 18)
#define SQL_EXTENSION_PRIV_EVENT          (1ULL << 19)
#define SQL_EXTENSION_PRIV_SHOW_CREATE_ROUTINE (1ULL << 20)

extern struct sql_extension_service_st {
  int (*check_access_func)(MYSQL_THD thd, unsigned long long privileges,
                           const char *db, const char *table);
} *sql_extension_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define sql_extension_check_access(thd, privileges, db, table) \
  sql_extension_service->check_access_func(thd, privileges, db, table)

#else

/**
  Check that the client has the privileges (SQL_EXTENSION_PRIV_*) on a
  database, or on a table of it when table is not NULL. On failure raises
  the usual access-denied error. db must be a name, table a name or NULL:
  they are checked as a statement's would be (ER_WRONG_DB_NAME,
  ER_WRONG_TABLE_NAME) and lower-cased as lower_case_table_names
  requires. privileges must be non-zero and made of SQL_EXTENSION_PRIV_*
  bits.
*/
int sql_extension_check_access(MYSQL_THD thd, unsigned long long privileges,
                               const char *db, const char *table);
#endif /* MYSQL_DYNAMIC_PLUGIN */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MYSQL_SERVICE_SQL_EXTENSION_INCLUDED */
