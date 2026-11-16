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

#ifndef MYSQL_SERVICE_SQL_EXTENSION_RESULT_INCLUDED
#define MYSQL_SERVICE_SQL_EXTENSION_RESULT_INCLUDED

/**
  @file include/mysql/service_sql_extension_result.h
  SQL extension result service: result sets and OK packets that the
  execute() function of a MariaDB_SQL_EXTENSION_PLUGIN sends to the client.
*/

/**
  @defgroup plugin_api_service_sql_extension_result SQL Extension Result Service
  @ingroup plugin_api_services

  Results of an SQL extension statement (see service_sql_extension.h for
  the rules common to the SQL extension services).

  A statement answers with one or more results, each a result set or an OK
  packet, sent with sql_extension_send_result_set() or
  sql_extension_send_ok(); or with an error.

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

  After an invalid call, sql_extension_client_flags() returns 0, which is
  also a valid result: execute() should then fail.
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

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

extern struct sql_extension_result_service_st {
  int (*send_result_set_func)(MYSQL_THD thd, size_t ncols,
                              const struct sql_extension_column *cols,
                              size_t nrows,
                              const struct sql_extension_cell *cells);
  int (*send_ok_func)(MYSQL_THD thd, unsigned long long affected_rows,
                      const char *message);
  unsigned int (*client_flags_func)(MYSQL_THD thd);
} *sql_extension_result_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define sql_extension_send_result_set(thd, ncols, cols, nrows, cells) \
  sql_extension_result_service->send_result_set_func(thd, ncols, cols, nrows, \
                                                     cells)
#define sql_extension_send_ok(thd, affected_rows, message) \
  sql_extension_result_service->send_ok_func(thd, affected_rows, message)
#define sql_extension_client_flags(thd) \
  sql_extension_result_service->client_flags_func(thd)

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

#endif /* MYSQL_SERVICE_SQL_EXTENSION_RESULT_INCLUDED */
