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

#ifndef MYSQL_SERVICE_SQL_EXTENSION_CLIENT_INCLUDED
#define MYSQL_SERVICE_SQL_EXTENSION_CLIENT_INCLUDED

/**
  @file include/mysql/service_sql_extension_client.h
  SQL extension client service: SQL that the execute() function of a
  MariaDB_SQL_EXTENSION_PLUGIN runs as the client.
*/

/**
  @defgroup plugin_api_service_sql_extension_client SQL Extension Client Service
  @ingroup plugin_api_services

  SQL run as the client by an SQL extension statement (see
  service_sql_extension.h for the rules common to the SQL extension
  services). For a plugin whose statements stand for SQL the client could
  run itself: the SQL gets the client's privileges, transaction, locks,
  binary logging, kill and timeout. A plugin whose SQL is work of its own
  uses the plain SQL service (service_sql.h) instead.
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

struct st_mysql;

extern struct sql_extension_client_service_st {
  struct st_mysql *(*connect_as_client_func)(MYSQL_THD thd,
                                             struct st_mysql *mysql);
  int (*forward_sql_func)(MYSQL_THD thd, const char *query, size_t length);
} *sql_extension_client_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define sql_extension_connect_as_client(thd, mysql) \
  sql_extension_client_service->connect_as_client_func(thd, mysql)
#define sql_extension_forward_sql(thd, query, length) \
  sql_extension_client_service->forward_sql_func(thd, query, length)

#else

/**
  Connect a MYSQL handle, from mysql_init() of the SQL service, that runs
  SQL on the client's connection, as the client:
  - with the client's privileges, current database and transaction;
  - binary-logged according to the client's sql_log_bin;
  - interrupted by KILL (QUERY or CONNECTION) and max_statement_time of
    the statement;
  - under LOCK TABLES, with the client's locked tables;
  - in utf8mb4: the SQL text is read as utf8mb4 and the rows, column
    names and messages come back in utf8mb4, whatever the client's
    character set, which each statement leaves as it was (SET NAMES on
    the handle has no lasting effect).
  Statements claimed by SQL extension plugins fail with
  ER_SQL_EXTENSION_NESTED. Warnings and errors of the SQL stay on the
  handle; the plugin decides what to pass on to the client, except after
  a kill (KILL, max_statement_time) or an error that rolled back
  the client's transaction (a deadlock, a lock wait timeout with
  innodb_rollback_on_timeout) and that no stored program in the SQL
  handled: the statement then fails with that error, after
  whatever the plugin sent, even if the plugin did not pass it on.
  The handle must be closed with mysql_close() before execute() returns:
  it belongs to that one execution of that statement, and a query on it
  from a later statement, or from another thread, fails with an error on
  the handle.

  Run the SQL of a statement through this connection, or through
  sql_extension_forward_sql(), not through the plain SQL service
  (mysql_real_connect_local()): that one runs as an internal user that
  skips the grants and does not write the binary log.

  @return mysql, or NULL on failure, with the error on the handle. An
          invalid call (see the start of this group) also raises
          ER_STD_INVALID_ARGUMENT on the calling thread's statement, or
          writes it to the error log when called from a thread without
          a THD. With mysql NULL there is no handle: the error is only
          raised (or logged).
*/
struct st_mysql *sql_extension_connect_as_client(MYSQL_THD thd,
                                                 struct st_mysql *mysql);
/**
  Run one SQL statement as the client, like sql_extension_connect_as_client(),
  and send its result to the client as it is: the result sets, metadata
  and rows of the server, or its OK packet. The text is utf8mb4; the
  results are in the client's character set, as for its own statements.
  A CALL sends every result of the procedure, so one that returns result
  sets needs a client that accepts several results, as when the client
  runs the CALL itself.
  The warnings of the statement, and its error if it fails, become the
  extension statement's. Like any other result, it is refused when the
  statement already sent one and the client does not accept several.
*/
int sql_extension_forward_sql(MYSQL_THD thd, const char *query,
                              size_t length);
#endif /* MYSQL_DYNAMIC_PLUGIN */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MYSQL_SERVICE_SQL_EXTENSION_CLIENT_INCLUDED */
