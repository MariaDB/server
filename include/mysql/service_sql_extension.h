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
  SQL extension service: what the execute() function of a
  MariaDB_SQL_EXTENSION_PLUGIN uses to answer the client.
*/

/**
  @defgroup plugin_api_service_sql_extension SQL Extension Service
  @ingroup plugin_api_services

  Functions for the execute() function of an SQL extension plugin
  (include/mysql/plugin_sql_extension.h). They may only be called from
  execute(), on the THD it was given.

  A statement answers with one or more results, each a result set or an OK
  packet, sent with sql_extension_send_result_set(), sql_extension_send_ok()
  or sql_extension_forward_sql(); or with an error raised with my_error() /
  my_printf_error(). When execute() returns 0 without sending anything, the
  client gets a plain OK. Notes and warnings are added to the statement
  with my_printf_error() and ME_NOTE or ME_WARNING.

  Several results are sent as a stored procedure sends them: every result
  but the last with SERVER_MORE_RESULTS_EXISTS. Only clients that accept
  several results get more than one (see sql_extension_client_flags()). An
  error after some results ends the statement: the client gets the results
  sent so far, then the error. A client that does not accept several
  results gets the error in place of the end of the result it was sent
  (a held OK is dropped), as when a SELECT fails after some rows. Errors
  in the use of the service (a second result the client does not accept,
  a malformed result set: no columns, a column without a name or of an
  unknown type, rows without cells, a cell that is not a value of its
  column, a DECIMAL column whose values need more than 65 digits or 38
  after the point; an OK message that is too long or ill-formed; or a
  result set by other means than the service)
  raise ER_SQL_EXTENSION_INVALID_RESULT.

  Functions returning int return 0 on success and non-zero after raising
  an error, which execute() should then return. Any other invalid call (a
  thd other than the one execute() was given, a NULL handle or query, an
  empty database or table name, privileges of 0 or with unknown bits)
  raises ER_STD_INVALID_ARGUMENT naming the argument, in all builds (not
  only an assertion). sql_extension_client_flags() then returns 0, which
  is also a valid result: execute() should then fail.
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

struct st_mysql;

/**
  Column types of a result set. Every cell is given as text, in the format
  below, and is checked before anything is sent. The precision of DECIMAL
  columns and the fractional seconds of TIME and DATETIME columns are taken
  from the cells.
*/
enum sql_extension_column_type
{
  SQL_EXTENSION_COLUMN_STRING= 0,     /**< well-formed utf8mb4 text */
  SQL_EXTENSION_COLUMN_BINARY= 1,     /**< raw bytes */
  SQL_EXTENSION_COLUMN_INTEGER= 2,    /**< signed 64-bit integer: -12 */
  SQL_EXTENSION_COLUMN_UNSIGNED= 3,   /**< unsigned 64-bit integer: 12 */
  SQL_EXTENSION_COLUMN_DOUBLE= 4,     /**< finite double: 1.5, -2e-3 */
  SQL_EXTENSION_COLUMN_DECIMAL= 5,    /**< exact number, up to 65 digits and
                                           38 after the point: -12.50 */
  SQL_EXTENSION_COLUMN_DATE= 6,       /**< 2026-11-06 */
  SQL_EXTENSION_COLUMN_TIME= 7,       /**< [-][D ]hh:mm:ss[.ffffff] */
  SQL_EXTENSION_COLUMN_DATETIME= 8,   /**< 2026-11-06 10:20:30[.ffffff] */
  SQL_EXTENSION_COLUMN_JSON= 9        /**< a valid JSON document, utf8mb4 */
};

struct sql_extension_column
{
  const char *name;                   /**< column name, treated as a column
                                           alias: leading spaces dropped,
                                           cut at 255 bytes, and each byte
                                           of a character outside the BMP
                                           (4 bytes in UTF-8) sent as a
                                           \x escape */
  int type;                           /**< enum sql_extension_column_type */
};

/** One cell: str == NULL is SQL NULL */
struct sql_extension_cell
{
  const char *str;
  size_t length;
};

/** What the client accepts, from sql_extension_client_flags() */
#define SQL_EXTENSION_CLIENT_MULTI_RESULTS    1 /**< several results */
#define SQL_EXTENSION_CLIENT_MULTI_STATEMENTS 2 /**< several statements in
                                                     one packet */

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
  int (*send_result_set_func)(MYSQL_THD thd, size_t ncols,
                              const struct sql_extension_column *cols,
                              size_t nrows,
                              const struct sql_extension_cell *cells);
  int (*send_ok_func)(MYSQL_THD thd, unsigned long long affected_rows,
                      const char *message);
  int (*check_access_func)(MYSQL_THD thd, unsigned long long privileges,
                           const char *db, const char *table);
  struct st_mysql *(*connect_as_client_func)(MYSQL_THD thd,
                                             struct st_mysql *mysql);
  int (*forward_sql_func)(MYSQL_THD thd, const char *query, size_t length);
  unsigned int (*client_flags_func)(MYSQL_THD thd);
} *sql_extension_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define sql_extension_send_result_set(thd, ncols, cols, nrows, cells) \
  sql_extension_service->send_result_set_func(thd, ncols, cols, nrows, cells)
#define sql_extension_send_ok(thd, affected_rows, message) \
  sql_extension_service->send_ok_func(thd, affected_rows, message)
#define sql_extension_check_access(thd, privileges, db, table) \
  sql_extension_service->check_access_func(thd, privileges, db, table)
#define sql_extension_connect_as_client(thd, mysql) \
  sql_extension_service->connect_as_client_func(thd, mysql)
#define sql_extension_forward_sql(thd, query, length) \
  sql_extension_service->forward_sql_func(thd, query, length)
#define sql_extension_client_flags(thd) \
  sql_extension_service->client_flags_func(thd)

#else

/**
  Send a result set: ncols columns and nrows rows, the cells row by row
  (nrows * ncols of them), each in the format of its column type.
*/
int sql_extension_send_result_set(MYSQL_THD thd, size_t ncols,
                                  const struct sql_extension_column *cols,
                                  size_t nrows,
                                  const struct sql_extension_cell *cells);

/**
  Send an OK packet with the number of affected rows and an optional
  message (NULL for none), shown by clients as "info". The message is
  utf8mb4 text of 511 bytes at most; a longer or ill-formed one raises
  ER_SQL_EXTENSION_INVALID_RESULT.
*/
int sql_extension_send_ok(MYSQL_THD thd, unsigned long long affected_rows,
                          const char *message);

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

/**
  Connect a MYSQL handle, from mysql_init() of the SQL service, that runs
  SQL on the client's connection, as the client:
  - with the client's privileges, current database and transaction;
  - binary-logged according to the client's sql_log_bin;
  - interrupted by KILL (QUERY or CONNECTION) and max_statement_time of
    the statement;
  - under LOCK TABLES, with the client's locked tables.
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
  and rows of the server, or its OK packet. A CALL sends every result of
  the procedure, so one that returns result sets needs a client that
  accepts several results, as when the client runs the CALL itself.
  The warnings of the statement, and its error if it fails, become the
  extension statement's. Like any other result, it is refused when the
  statement already sent one and the client does not accept several.
*/
int sql_extension_forward_sql(MYSQL_THD thd, const char *query,
                              size_t length);

/**
  What the client accepts in this statement: SQL_EXTENSION_CLIENT_*.
  MULTI_RESULTS: several results (CLIENT_MULTI_RESULTS, or
  CLIENT_PS_MULTI_RESULTS for the binary protocol). MULTI_STATEMENTS: the
  client sent the packet allowing several statements in it
  (CLIENT_MULTI_STATEMENTS), never set in a prepared statement. The server
  splits a packet at the end of a claimed statement only with it; a plugin
  whose statement holds several statements of its own (a script) should
  likewise refuse to run more than one without it.
*/
unsigned int sql_extension_client_flags(MYSQL_THD thd);

#endif /* MYSQL_DYNAMIC_PLUGIN */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MYSQL_SERVICE_SQL_EXTENSION_INCLUDED */
