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

/*
  Test plugins for the SQL extension framework, used by
  mysql-test/suite/sql_extension. One library, several plugins:

  test_sql_extension          claims "TEST SQLEXT", "WITH SQLEXT" and
                              "CREATE PROCEDURE SQLEXT". The last two start
                              valid SQL (a CTE, a procedure named SQLEXT),
                              for statements the parser rejects in other
                              ways than a plain syntax error; "WITH
                              SQLEXT" is also a carrier the query cache
                              looks up. The word after the prefix
                              selects what execute() does:
                                OK (or nothing)  success, the server sends OK
                                ERROR            raises an error
                                FAIL             fails without an error
                                ECHO             raises an error showing the
                                                 exact text it was given
                                ROWS             sends a result set
                                EMPTY            sends a result set with
                                                 columns s, i and d (DECIMAL)
                                                 and no rows
                                NULLS            the same with two rows of
                                                 NULLs only
                                TYPES            sends a result set with one
                                                 column of every type not
                                                 covered by ROWS
                                INFO [MAX|LONG|ILL_FORMED]
                                                 sends an OK with 3 affected
                                                 rows and a message; or a
                                                 message of 511 bytes
                                                 (the longest allowed), of
                                                 512 bytes, or of ill-formed
                                                 utf8mb4: both refused
                                WARN             adds a note and a warning
                                ACCESS [<priv>] <db> [<t>]
                                                 checks a privilege (SELECT
                                                 if none) on db or db.t;
                                                 NOPRIV passes none, BADBIT
                                                 an unknown bit, and '' an
                                                 empty table name
                                TWICE            sends OK twice
                                MULTI            sends rows, an OK, rows
                                MULTI ERROR      sends rows, then an error
                                FLAGS            sends the client flags
                                FORWARD <stmt>   runs <stmt> as the client,
                                                 its result goes to the
                                                 client as it is;
                                                 FORWARD '<text>' forwards
                                                 <text> as it is, ';'
                                                 included
                                SCRIPT '<s1>; <s2>...'
                                                 forwards each statement
                                                 (strings in them in double
                                                 quotes);
                                                 several need
                                                 CLIENT_MULTI_STATEMENTS;
                                                 debug sync point
                                                 test_sql_extension_script
                                                 between two statements
                                BAD <type> <text>
                                                 sends <text> as a cell of a
                                                 column of <type> (STRING,
                                                 INTEGER, DOUBLE, DECIMAL,
                                                 DATE, TIME, DATETIME,
                                                 JSON...): refused if it is
                                                 not a valid value; the text
                                                 ILL_FORMED stands for
                                                 ill-formed utf8mb4
                                BAD2 <type> <text1> <text2>
                                                 the same with two rows
                                BADSET NOCOLS|NONAME|BADTYPE|NOCELLS
                                                 sends a malformed result
                                                 set: no columns, a column
                                                 without a name, a column
                                                 of an unknown type, a row
                                                 without cells
                                BADTHD           calls the service with no
                                                 thd
                                BADCALL MYSQL|HANDLE|QUERY|FORWARD
                                                 invalid calls: connects a
                                                 NULL handle, connects one
                                                 with no thd (the handle's
                                                 error added as a note),
                                                 forwards a NULL query,
                                                 forwards with no thd
                                SQL <stmt>       runs <stmt> as the client,
                                                 relays its result or error
                                KEEP <stmt>      the same, on a connection
                                                 kept open afterwards;
                                                 REUSE <stmt> runs on it,
                                                 KEEP CLOSE closes it
                                SQL2 <stmt>      runs <stmt> twice as the
                                                 client, relays the second;
                                                 sync point in between:
                                                 test_sql_extension_sql2
                                LOCAL SQL <stmt> runs <stmt> through the plain
                                                 SQL service, relays its
                                                 error; LOCAL SQL2 <stmt>
                                                 runs it twice, as SQL2
                                [LOCAL] RAISE <stmt>
                                                 raises an error, then runs
                                                 <stmt> as the client (or
                                                 through the plain SQL
                                                 service), ignoring its
                                                 result, and fails: the
                                                 client gets the raised
                                                 error
                                [LOCAL] SWALLOW [OK|ROWS] <stmt>
                                                 runs <stmt> as the client
                                                 (or through the plain SQL
                                                 service), ignores its
                                                 result and its error, and
                                                 succeeds; with OK or ROWS
                                                 it then sends an OK or a
                                                 row. <stmt> may be a quoted
                                                 script, '<s1>; <s2>...',
                                                 run statement by statement
                                NOTHD            connects as the client from
                                                 a thread of its own, with
                                                 no THD, and raises the
                                                 error the handle got
                                THREAD <stmt>    connects as the client,
                                                 runs <stmt> on the handle
                                                 from a thread of its own
                                                 during the statement, and
                                                 raises the error the handle
                                                 got
  test_sql_extension_overlap  claims "TEST OVERLAP FREE" and "TEST SQLEXT
                              OVERLAP": refused while test_sql_extension
                              is installed, the free prefix too; counts its
                              init() and deinit() calls in status
                              variables of test_sql_extension
  test_sql_extension_bad      invalid descriptor (no execute()): refused
  test_sql_extension_empty    invalid descriptor (neither prefixes nor
                              recognize()): refused
  test_sql_extension_bad_prefix
                              invalid descriptor (the prefix "TEST 1BAD" is
                              not keywords): refused
  test_sql_extension_mixed    invalid descriptor (both prefixes and
                              recognize()): refused
  test_sql_extension_bad_priority
                              invalid descriptor (a priority with
                              prefixes): refused
  test_sql_extension_bad_init claims "TEST BADINIT", but its init() fails:
                              refused; counts its init() and deinit()
                              calls in status variables of
                              test_sql_extension
  test_sql_extension_recognizer
                              recognizer: claims "TEST RECOGNIZE ..." up to
                              the first ';', or only up to the word SHORT
                              if there is one, or past the end of the
                              statement with the word PAST (a bug the
                              server refuses); ECHO as above. In
                              "TEST RECOGNIZE WAIT", recognize() stops at
                              the debug sync point
                              test_sql_extension_recognize
  test_sql_extension_priority recognizer with priority 10: claims
                              "TEST RECOGNIZE ..." unless the word LOW is
                              in it, so it takes precedence over
                              test_sql_extension_recognizer (priority 0)
                              whenever it is installed; also "TEST SQLEXT
                              PRIORITY" (owned by test_sql_extension's
                              prefix when that plugin is installed); sends
                              OK with its name as the message
*/

#include <my_global.h>
#include <sql_plugin.h>                         // st_plugin_int
#include <mysql/plugin_sql_extension.h>
#include <mysqld_error.h>
#include <string.h>
#include <stdio.h>
#include <thread>

#ifndef STRING_WITH_LEN                         // used by DEBUG_SYNC()
#define STRING_WITH_LEN(X) (X), ((size_t) (sizeof(X) - 1))
#endif

static int execute_noop(MYSQL_THD, const char *, size_t)
{
  return 0;
}


static bool is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char upper(char c)
{
  return (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
}

/*
  Find `word` (uppercase) in the text, case-insensitively, as a whole word,
  and return the position after it, or NULL.
*/
static const char *after_word(const char *q, const char *end,
                              const char *word)
{
  size_t len= strlen(word);
  for (const char *p= q; p + len <= end; p++)
  {
    if (p > q && !is_space(p[-1]))
      continue;
    size_t i= 0;
    while (i < len && upper(p[i]) == word[i])
      i++;
    if (i == len && (p + len == end || is_space(p[len]) || p[len] == ';'))
      return p + len;
  }
  return NULL;
}

static const char *skip_space(const char *p, const char *end)
{
  while (p < end && is_space(*p))
    p++;
  return p;
}

/* The next word after `p`: [*word, *word + *len) */
static void next_word(const char *p, const char *end,
                      const char **word, size_t *len)
{
  p= skip_space(p, end);
  *word= p;
  while (p < end && !is_space(*p) && *p != ';')
    p++;
  *len= (size_t) (p - *word);
}

static bool word_is(const char *w, size_t len, const char *kw)
{
  if (strlen(kw) != len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (upper(w[i]) != kw[i])
      return false;
  return true;
}

static int echo(const char *query, size_t length)
{
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: [%.*s]", 0,
                  (int) length, query);
  return 1;
}

/* Send what a statement run on a local connection produced: rows or OK */
static int relay_result(MYSQL_THD thd, MYSQL *mysql, MYSQL_RES *res)
{
  if (!res)
    return sql_extension_send_ok(thd, mysql_affected_rows(mysql), NULL);

  unsigned int ncols= mysql_num_fields(res);
  size_t nrows= (size_t) mysql_num_rows(res);
  MYSQL_FIELD *fields= mysql_fetch_fields(res);
  sql_extension_column *cols= new sql_extension_column[ncols];
  sql_extension_cell *cells= new sql_extension_cell[nrows * ncols + 1];
  for (unsigned int c= 0; c < ncols; c++)
    cols[c]= { fields[c].name, SQL_EXTENSION_COLUMN_STRING };
  sql_extension_cell *cell= cells;
  while (MYSQL_ROW row= mysql_fetch_row(res))
  {
    unsigned long *lengths= mysql_fetch_lengths(res);
    for (unsigned int c= 0; c < ncols; c++, cell++)
      *cell= { row[c], row[c] ? (size_t) lengths[c] : 0 };
  }
  int rc= sql_extension_send_result_set(thd, ncols, cols, nrows, cells);
  delete[] cells;
  delete[] cols;
  return rc;
}

static int out_of_memory()
{
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: out of memory", 0);
  return 1;
}

/* Make the error of a local connection the statement's */
static int relay_error(MYSQL *mysql)
{
  my_printf_error(mysql_errno(mysql), "%s", 0, mysql_error(mysql));
  return 1;
}

/*
  A local connection on the client's connection: as the client, or through
  the plain SQL service (no grants, no binary log, a THD of its own under
  LOCK TABLES). NULL, with the error raised, on failure.
*/
static MYSQL *connect_handle(MYSQL_THD thd, bool as_client)
{
  MYSQL *mysql= mysql_init(NULL);
  if (mysql && (as_client ? sql_extension_connect_as_client(thd, mysql)
                          : mysql_real_connect_local(mysql)))
    return mysql;
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: connect: %s", 0,
                  mysql ? mysql_error(mysql) : "out of memory");
  if (mysql)
    mysql_close(mysql);
  return NULL;
}

/*
  Run one statement on a local connection; with relay, send its rows or OK
  to the client. A failure, also one while reading the rows (e.g. killed),
  becomes the statement's error.
*/
static int query_and_relay(MYSQL_THD thd, MYSQL *mysql, const char *sql,
                           size_t length, bool relay)
{
  if (mysql_real_query(mysql, sql, (unsigned long) length))
    return relay_error(mysql);
  MYSQL_RES *res= mysql_store_result(mysql);
  if (!res && mysql_errno(mysql))
    return relay_error(mysql);
  int rc= relay ? relay_result(thd, mysql, res) : 0;
  if (res)
    mysql_free_result(res);
  return rc;
}

/*
  Run SQL on the client's connection, times times (SQL2 runs it twice);
  as the client, the result of the last run is relayed
*/
static int run_sql(MYSQL_THD thd, const char *sql, size_t length,
                   bool as_client, int times)
{
  MYSQL *mysql= connect_handle(thd, as_client);
  if (!mysql)
    return 1;
  int rc= 0;
  while (times-- && !rc)
  {
    rc= query_and_relay(thd, mysql, sql, length, as_client && !times);
    if (!rc && times)
      DEBUG_SYNC(thd, "test_sql_extension_sql2");
  }
  mysql_close(mysql);
  return rc;
}

/*
  KEEP <stmt> runs <stmt> on an as-client connection that is kept open
  across statements; REUSE <stmt> runs <stmt> on it; KEEP CLOSE closes it
*/
static MYSQL *kept_handle;

/* A handle a test left kept is closed at UNINSTALL, not leaked */
static int main_deinit(void *)
{
  if (kept_handle)
    mysql_close(kept_handle);
  kept_handle= NULL;
  return 0;
}

static int run_kept(MYSQL_THD thd, const char *sql, size_t length, bool open)
{
  if (open)
  {
    if (kept_handle)
      mysql_close(kept_handle);
    if (!(kept_handle= connect_handle(thd, true)))
      return 1;
  }
  if (!kept_handle)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: nothing kept", 0);
    return 1;
  }
  return query_and_relay(thd, kept_handle, sql, length, true);
}

static int send_one_row(MYSQL_THD thd, const char *name, const char *value)
{
  sql_extension_column col= { name, SQL_EXTENSION_COLUMN_STRING };
  sql_extension_cell cell= { value, strlen(value) };
  return sql_extension_send_result_set(thd, 1, &col, 1, &cell);
}

static int send_multi(MYSQL_THD thd, const char *p, const char *end)
{
  const char *word;
  size_t len;
  next_word(p, end, &word, &len);
  if (word_is(word, len, "ERROR"))
  {
    if (send_one_row(thd, "first", "1"))
      return 1;
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: error after rows",
                    0);
    return 1;
  }
  return send_one_row(thd, "first", "1") ||
         sql_extension_send_ok(thd, 2, "test_sql_extension: second") ||
         send_one_row(thd, "third", "3");
}

static int send_flags(MYSQL_THD thd)
{
  static const sql_extension_column cols[]=
  {
    { "multi_results", SQL_EXTENSION_COLUMN_INTEGER },
    { "multi_statements", SQL_EXTENSION_COLUMN_INTEGER }
  };
  unsigned int flags= sql_extension_client_flags(thd);
  sql_extension_cell cells[]=
  {
    { (flags & SQL_EXTENSION_CLIENT_MULTI_RESULTS) ? "1" : "0", 1 },
    { (flags & SQL_EXTENSION_CLIENT_MULTI_STATEMENTS) ? "1" : "0", 1 }
  };
  return sql_extension_send_result_set(thd, 2, cols, 1, cells);
}

/*
  Forward each ';'-separated statement of a script written as one quoted
  string, '<s1>; <s2>...', as a plugin running a script would. The quotes
  keep its ';' inside the extension statement, which otherwise ends at the
  first one. The split is naive: no quote and no ';' inside the
  statements (write their strings in double quotes).
*/
static int forward_script(MYSQL_THD thd, const char *p, const char *end)
{
  while (end > p && is_space(end[-1]))
    end--;
  if (end - p < 2 || *p != '\'' || end[-1] != '\'')
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: SCRIPT needs "
                    "a quoted script", 0);
    return 1;
  }
  p++;
  end--;
  const char *semicolon= (const char *) memchr(p, ';', (size_t) (end - p));
  if (semicolon && skip_space(semicolon + 1, end) != end &&
      !(sql_extension_client_flags(thd) &
        SQL_EXTENSION_CLIENT_MULTI_STATEMENTS))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: several "
                    "statements need CLIENT_MULTI_STATEMENTS", 0);
    return 1;
  }
  while (p < end)
  {
    const char *stmt_end= (const char *) memchr(p, ';', (size_t) (end - p));
    if (!stmt_end)
      stmt_end= end;
    if (skip_space(p, stmt_end) != stmt_end &&
        sql_extension_forward_sql(thd, p, (size_t) (stmt_end - p)))
      return 1;
    p= stmt_end + 1;
    /* Lets a test act between two statements of the script */
    if (p < end)
      DEBUG_SYNC(thd, "test_sql_extension_script");
  }
  return 0;
}

static int send_rows(MYSQL_THD thd)
{
  static const sql_extension_column cols[]=
  {
    { "s", SQL_EXTENSION_COLUMN_STRING },
    { "b", SQL_EXTENSION_COLUMN_BINARY },
    { "i", SQL_EXTENSION_COLUMN_INTEGER },
    { "u", SQL_EXTENSION_COLUMN_UNSIGNED }
  };
  static const sql_extension_cell cells[]=
  {
    { "caf\xC3\xA9", 5 }, { "b\xFFn", 3 }, { "-9223372036854775808", 20 },
    { "18446744073709551615", 20 },
    { "", 0 }, { NULL, 0 }, { "0", 1 }, { NULL, 0 }
  };
  return sql_extension_send_result_set(thd, 4, cols, 2, cells);
}

/* Columns s, i and d with no rows (EMPTY), or two rows of NULLs (NULLS) */
static int send_no_values(MYSQL_THD thd, bool nulls)
{
  static const sql_extension_column cols[]=
  {
    { "s", SQL_EXTENSION_COLUMN_STRING },
    { "i", SQL_EXTENSION_COLUMN_INTEGER },
    { "d", SQL_EXTENSION_COLUMN_DECIMAL }
  };
  static const sql_extension_cell cells[]=
  { { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 },
    { NULL, 0 } };
  return sql_extension_send_result_set(thd, 3, cols, nulls ? 2 : 0, cells);
}

/* One column of each type that ROWS does not cover, and a row of NULLs */
static int send_types(MYSQL_THD thd)
{
  static const sql_extension_column cols[]=
  {
    { "d", SQL_EXTENSION_COLUMN_DOUBLE },
    { "n", SQL_EXTENSION_COLUMN_DECIMAL },
    { "dt", SQL_EXTENSION_COLUMN_DATE },
    { "t", SQL_EXTENSION_COLUMN_TIME },
    { "ts", SQL_EXTENSION_COLUMN_DATETIME },
    { "j", SQL_EXTENSION_COLUMN_JSON }
  };
  static const sql_extension_cell cells[]=
  {
    { "1.5", 3 }, { "-12.5", 5 }, { "2026-11-06", 10 },
    { "-838:59:59", 10 }, { "2026-11-06 10:20:30.5", 21 },
    { "{\"a\": [1, 2]}", 13 },

    { "-2e-3", 5 }, { "3.125", 5 }, { "0000-00-00", 10 },
    { "1 02:03:04.123456", 17 }, { "2026-11-06 10:20:30", 19 },
    { "null", 4 },

    { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 },
    { NULL, 0 }
  };
  return sql_extension_send_result_set(thd, 6, cols, 3, cells);
}

/*
  BAD <type> <text>: a one-cell result set of the named type; BAD2
  (two_rows) sends the cell in two rows
*/
static int send_bad(MYSQL_THD thd, const char *p, const char *end,
                    bool two_rows)
{
  static const char *const names[]=
  {
    "STRING", "BINARY", "INTEGER", "UNSIGNED", "DOUBLE", "DECIMAL", "DATE",
    "TIME", "DATETIME", "JSON", NULL
  };
  const char *word;
  size_t len;
  next_word(p, end, &word, &len);
  int type= 0;
  while (names[type] && !word_is(word, len, names[type]))
    type++;
  if (!names[type])
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: no type '%.*s'", 0,
                    (int) len, word);
    return 1;
  }
  sql_extension_column col= { "c", type };
  if (two_rows)
  {
    /* Two words, one cell each */
    sql_extension_cell two[2];
    for (sql_extension_cell &c : two)
    {
      next_word(word + len, end, &word, &len);
      c= { word, len };
    }
    return sql_extension_send_result_set(thd, 1, &col, 2, two);
  }
  /* The text is everything after one space following the type */
  const char *text= word + len;
  if (text < end)
    text++;
  sql_extension_cell cell= { text, (size_t) (end - text) };
  /* A statement cannot carry ill-formed utf8mb4: a word stands for it */
  next_word(text, end, &word, &len);
  if (word_is(word, len, "ILL_FORMED"))
    cell= { "caf\xC3", 4 };                      // a truncated 'é'
  return sql_extension_send_result_set(thd, 1, &col, 1, &cell);
}

static int send_info(MYSQL_THD thd, const char *p, const char *end)
{
  const char *word;
  size_t len;
  char text[513];
  next_word(p, end, &word, &len);
  if (word_is(word, len, "MAX") || word_is(word, len, "LONG"))
  {
    size_t n= word_is(word, len, "MAX") ? 511 : 512;
    memset(text, 'x', n);
    text[n]= 0;
    return sql_extension_send_ok(thd, 0, text);
  }
  if (word_is(word, len, "ILL_FORMED"))
    return sql_extension_send_ok(thd, 0, "caf\xC3");  // a truncated 'é'
  return sql_extension_send_ok(thd, 3, "test_sql_extension: info");
}

static int check_access(MYSQL_THD thd, const char *p, const char *end)
{
  static const struct { const char *name; unsigned long long priv; } privs[]=
  {
    { "SELECT",         SQL_EXTENSION_PRIV_SELECT },
    { "INSERT",         SQL_EXTENSION_PRIV_INSERT },
    { "REFERENCES",     SQL_EXTENSION_PRIV_REFERENCES },
    { "GRANT_OPTION",   SQL_EXTENSION_PRIV_GRANT_OPTION },
    { "DELETE_HISTORY", SQL_EXTENSION_PRIV_DELETE_HISTORY },
    { "CREATE_TEMPORARY_TABLES",
                        SQL_EXTENSION_PRIV_CREATE_TEMPORARY_TABLES },
    { "LOCK_TABLES",    SQL_EXTENSION_PRIV_LOCK_TABLES },
    { "EVENT",          SQL_EXTENSION_PRIV_EVENT },
    { "SHOW_CREATE_ROUTINE", SQL_EXTENSION_PRIV_SHOW_CREATE_ROUTINE },
    { "NOPRIV",         0 },
    { "BADBIT",         1ULL << 62 },
  };
  char db[256], table[256];
  const char *word;
  size_t len;
  unsigned long long priv= SQL_EXTENSION_PRIV_SELECT;
  next_word(p, end, &word, &len);
  for (const auto &pr : privs)
    if (word_is(word, len, pr.name))
    {
      priv= pr.priv;
      next_word(word + len, end, &word, &len);
      break;
    }
  snprintf(db, sizeof(db), "%.*s", (int) len, word);
  next_word(word + len, end, &word, &len);
  snprintf(table, sizeof(table), "%.*s", (int) len, word);
  if (!strcmp(table, "''"))
    table[0]= 0;
  return sql_extension_check_access(thd, priv, db, len ? table : NULL);
}

/*
  Malformed result sets: no columns, a column without a name, a bad type,
  a row without cells
*/
static int send_badset(MYSQL_THD thd, const char *p, const char *end)
{
  const char *word;
  size_t len;
  next_word(p, end, &word, &len);
  sql_extension_cell cell= { "x", 1 };
  if (word_is(word, len, "NOCOLS"))
    return sql_extension_send_result_set(thd, 0, NULL, 0, NULL);
  if (word_is(word, len, "NONAME"))
  {
    sql_extension_column col= { NULL, SQL_EXTENSION_COLUMN_STRING };
    return sql_extension_send_result_set(thd, 1, &col, 1, &cell);
  }
  if (word_is(word, len, "BADTYPE"))
  {
    sql_extension_column col= { "c", 99 };
    return sql_extension_send_result_set(thd, 1, &col, 1, &cell);
  }
  if (word_is(word, len, "NOCELLS"))
  {
    sql_extension_column col= { "c", SQL_EXTENSION_COLUMN_STRING };
    return sql_extension_send_result_set(thd, 1, &col, 1, NULL);
  }
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: unknown '%.*s'", 0,
                  (int) len, word);
  return 1;
}

static int execute_main(MYSQL_THD thd, const char *query, size_t length)
{
  const char *end= query + length;
  const char *p= after_word(query, end, "SQLEXT");
  const char *word;
  size_t len;
  next_word(p ? p : end, end, &word, &len);

  if (len == 0 || word_is(word, len, "OK"))
    return 0;
  if (word_is(word, len, "ERROR"))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: error", 0);
    return 1;
  }
  if (word_is(word, len, "FAIL"))
    return 1;
  if (word_is(word, len, "ECHO"))
    return echo(query, length);
  if (word_is(word, len, "EMPTY") || word_is(word, len, "NULLS"))
    return send_no_values(thd, word_is(word, len, "NULLS"));
  if (word_is(word, len, "ROWS"))
    return send_rows(thd);
  if (word_is(word, len, "TYPES"))
    return send_types(thd);
  if (word_is(word, len, "BAD") || word_is(word, len, "BAD2"))
    return send_bad(thd, word + len, end, len == 4);
  if (word_is(word, len, "BADSET"))
    return send_badset(thd, word + len, end);
  if (word_is(word, len, "BADTHD"))
    return sql_extension_send_ok(NULL, 0, NULL);
  if (word_is(word, len, "BADCALL"))
  {
    next_word(word + len, end, &word, &len);
    if (word_is(word, len, "MYSQL"))
      return sql_extension_connect_as_client(thd, NULL) ? 0 : 1;
    if (word_is(word, len, "QUERY"))
      return sql_extension_forward_sql(thd, NULL, 0);
    if (word_is(word, len, "FORWARD"))
      return sql_extension_forward_sql(NULL, "SELECT 1", 8);
    if (word_is(word, len, "HANDLE"))
    {
      MYSQL *mysql= mysql_init(NULL);
      if (!mysql)
        return out_of_memory();
      if (sql_extension_connect_as_client(NULL, mysql))
        return 0;
      my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: handle: %s",
                      ME_NOTE, mysql_error(mysql));
      mysql_close(mysql);
      return 1;
    }
  }
  if (word_is(word, len, "INFO"))
    return send_info(thd, word + len, end);
  if (word_is(word, len, "WARN"))
  {
    my_printf_error(1, "test_sql_extension: note", ME_NOTE);
    my_printf_error(2, "test_sql_extension: warning", ME_WARNING);
    return 0;
  }
  if (word_is(word, len, "ACCESS"))
    return check_access(thd, word + len, end);
  if (word_is(word, len, "TWICE"))
    return sql_extension_send_ok(thd, 0, NULL) ||
           sql_extension_send_ok(thd, 0, NULL);
  if (word_is(word, len, "MULTI"))
    return send_multi(thd, word + len, end);
  if (word_is(word, len, "FLAGS"))
    return send_flags(thd);
  if (word_is(word, len, "FORWARD"))
  {
    const char *sql= skip_space(word + len, end);
    /* Quoted: the text as it is, as the claim ends at the first ';' */
    const char *sql_end= end;
    while (sql_end > sql && is_space(sql_end[-1]))
      sql_end--;
    if (sql_end - sql >= 2 && *sql == '\'' && sql_end[-1] == '\'')
    {
      sql++;
      sql_end--;
    }
    else
      sql_end= end;
    return sql_extension_forward_sql(thd, sql, (size_t) (sql_end - sql));
  }
  if (word_is(word, len, "SCRIPT"))
    return forward_script(thd, skip_space(word + len, end), end);
  if (word_is(word, len, "KEEP") || word_is(word, len, "REUSE"))
  {
    bool open= len == 4;
    const char *sql= skip_space(word + len, end);
    if (open && after_word(word, end, "CLOSE") == end)
    {
      if (kept_handle)
        mysql_close(kept_handle);
      kept_handle= NULL;
      return 0;
    }
    return run_kept(thd, sql, (size_t) (end - sql), open);
  }
  bool as_client= true;
  if (word_is(word, len, "LOCAL"))
  {
    as_client= false;
    next_word(word + len, end, &word, &len);
  }
  if (as_client && word_is(word, len, "NOTHD"))
  {
    MYSQL *mysql= mysql_init(NULL);
    if (!mysql)
      return out_of_memory();
    MYSQL *connected= NULL;
    std::thread([&] { connected= sql_extension_connect_as_client(thd, mysql); })
      .join();
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: %s: %s", 0,
                    connected ? "connected" : "refused", mysql_error(mysql));
    mysql_close(mysql);
    return 1;
  }
  if (as_client && word_is(word, len, "THREAD"))
  {
    const char *sql= skip_space(word + len, end);
    MYSQL *mysql= connect_handle(thd, true);
    if (!mysql)
      return 1;
    int failed= 0;
    std::thread([&] {
      failed= mysql_real_query(mysql, sql, (unsigned long) (end - sql));
    }).join();
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: %s: %s", 0,
                    failed ? "refused" : "ran", mysql_error(mysql));
    mysql_close(mysql);
    return 1;
  }
  if (word_is(word, len, "SWALLOW"))
  {
    const char *sql= word + len;
    const char *then;
    size_t then_len;
    next_word(sql, end, &then, &then_len);
    bool send_ok= word_is(then, then_len, "OK");
    bool send_row= word_is(then, then_len, "ROWS");
    if (send_ok || send_row)
      sql= then + then_len;
    sql= skip_space(sql, end);
    MYSQL *mysql= mysql_init(NULL);
    if (!mysql)
      return out_of_memory();
    if (as_client ? sql_extension_connect_as_client(thd, mysql) != NULL
                  : mysql_real_connect_local(mysql) != NULL)
    {
      /* A quoted script: each statement, ignoring the error of each */
      const char *stop= end;
      while (stop > sql && is_space(stop[-1]))
        stop--;
      bool script= stop - sql >= 2 && *sql == '\'' && stop[-1] == '\'';
      if (script)
      {
        sql++;
        stop--;
      }
      while (sql < stop)
      {
        const char *stmt_end= script ? (const char *)
          memchr(sql, ';', (size_t) (stop - sql)) : NULL;
        if (!stmt_end)
          stmt_end= stop;
        if (skip_space(sql, stmt_end) != stmt_end &&
            !mysql_real_query(mysql, sql, (unsigned long) (stmt_end - sql)))
          mysql_free_result(mysql_store_result(mysql));
        sql= stmt_end + 1;
      }
    }
    mysql_close(mysql);
    if (send_ok)
      return sql_extension_send_ok(thd, 0, NULL);
    if (send_row)
      return send_one_row(thd, "r", "after");
    return 0;
  }
  if (word_is(word, len, "RAISE"))
  {
    const char *sql= skip_space(word + len, end);
    my_printf_error(ER_UNKNOWN_ERROR,
                    "test_sql_extension: raised before its SQL", 0);
    MYSQL *mysql= mysql_init(NULL);
    if (!mysql)
      return 1;
    if (as_client ? sql_extension_connect_as_client(thd, mysql) != NULL
                  : mysql_real_connect_local(mysql) != NULL)
      if (!mysql_real_query(mysql, sql, (unsigned long) (end - sql)))
        mysql_free_result(mysql_store_result(mysql));
    mysql_close(mysql);
    return 1;
  }
  if (word_is(word, len, "SQL") || word_is(word, len, "SQL2"))
  {
    const char *sql= skip_space(word + len, end);
    return run_sql(thd, sql, (size_t) (end - sql), as_client,
                   len == 4 ? 2 : 1);
  }
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: unknown '%.*s'", 0,
                  (int) len, word);
  return 1;
}

static int execute_recognizer(MYSQL_THD, const char *query, size_t length)
{
  if (after_word(query, query + length, "ECHO"))
    return echo(query, length);
  return 0;
}


/*
  "WITH SQLEXT" and "CREATE PROCEDURE SQLEXT" start valid SQL, so a
  statement with them can fail to parse for a reason the dispatch must
  respect. "WITH SQLEXT" is also a carrier the query cache looks up (its
  text starts with WIT).
*/
static const char *const main_prefixes[]= { "TEST SQLEXT", "WITH SQLEXT",
                                            "CREATE PROCEDURE SQLEXT",
                                            nullptr };

static st_mariadb_sql_extension main_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  main_prefixes,
  nullptr,
  execute_main,
  0
};


static const char *const overlap_prefixes[]=
  { "TEST OVERLAP FREE", "TEST SQLEXT OVERLAP", nullptr };

static st_mariadb_sql_extension overlap_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  overlap_prefixes,
  nullptr,
  execute_noop,
  0
};


static const char *const bad_prefixes[]= { "TEST SQLEXT BAD", nullptr };

static st_mariadb_sql_extension bad_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_prefixes,
  nullptr,
  nullptr,                                      // no execute(): invalid
  0
};


/* Neither prefixes nor recognize(): invalid */
static st_mariadb_sql_extension empty_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  nullptr,
  nullptr,
  execute_noop,
  0
};


static bool starts_with(const char *query, size_t length, const char *kw)
{
  size_t len= strlen(kw);
  if (length < len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (upper(query[i]) != kw[i])
      return false;
  return true;
}

/*
  Claims "TEST RECOGNIZE" up to the first ';', or only up to the word SHORT:
  then what follows the claim decides whether it stands.
*/
static size_t recognize_test(const char *query, size_t length)
{
  if (!starts_with(query, length, "TEST RECOGNIZE"))
    return 0;
  const char *semicolon= (const char *) memchr(query, ';', length);
  size_t claimed= semicolon ? (size_t) (semicolon - query) : length;
  if (const char *end= after_word(query + 14, query + claimed, "SHORT"))
    claimed= (size_t) (end - query);
  if (after_word(query + 14, query + claimed, "PAST"))
    return length + 10;
  /*
    Lets a test run INSTALL or UNINSTALL while a recognizer runs. This
    breaks the rule that a recognizer does not touch the THD, on purpose
    and for the test only: the sync point needs the THD.
  */
  if (after_word(query + 14, query + claimed, "WAIT"))  // 14: the prefix
    /* NULL: 12.3 has no THD service; debug_sync() uses the current THD */
    DEBUG_SYNC(nullptr, "test_sql_extension_recognize");
  return claimed;
}

static st_mariadb_sql_extension recognizer_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  nullptr,
  recognize_test,
  execute_recognizer,
  0
};


/* A prefix that is not keywords: invalid */
static const char *const bad_prefix[]= { "TEST 1BAD", nullptr };

static st_mariadb_sql_extension bad_prefix_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_prefix,
  nullptr,
  execute_noop,
  0
};


/* Both prefixes and recognize(): invalid */
static const char *const mixed_prefixes[]= { "TEST MIXED", nullptr };

static st_mariadb_sql_extension mixed_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  mixed_prefixes,
  recognize_test,
  execute_recognizer,
  0
};


/* A priority with prefixes: invalid */
static const char *const bad_priority_prefixes[]=
  { "TEST BADPRIORITY", nullptr };

static st_mariadb_sql_extension bad_priority_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_priority_prefixes,
  nullptr,
  execute_noop,
  1
};


/*
  Like recognize_test(), but leaves "TEST RECOGNIZE LOW ..." to others.
  Also claims "TEST SQLEXT PRIORITY", a text test_sql_extension owns by its
  prefix: a claimed prefix comes before any recognizer.
*/
static size_t recognize_priority(const char *query, size_t length)
{
  size_t claimed= recognize_test(query, length);
  if (claimed)
  {
    /* 14: the length of "TEST RECOGNIZE"; PAST claims beyond the end */
    const char *end= query + (claimed < length ? claimed : length);
    return after_word(query + 14, end, "LOW") ? 0 : claimed;
  }
  return starts_with(query, length, "TEST SQLEXT PRIORITY") ? length : 0;
}

static int execute_priority(MYSQL_THD thd, const char *, size_t)
{
  return sql_extension_send_ok(thd, 0, "test_sql_extension_priority");
}

static st_mariadb_sql_extension priority_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  nullptr,
  recognize_priority,
  execute_priority,
  10
};


/*
  init() and deinit() of test_sql_extension_overlap, counted: a plugin
  refused after its init() succeeded must get its deinit() too. Shown by
  test_sql_extension as status variables. init() keeps its state in
  st_plugin_int::data, as daemon_example does, and deinit() counts only
  if the server left it there.
*/
static int overlap_inits= 0, overlap_deinits= 0;

static int overlap_init(void *p)
{
  static_cast<st_plugin_int *>(p)->data= &overlap_inits;
  overlap_inits++;
  return 0;
}

static int overlap_deinit(void *p)
{
  if (static_cast<st_plugin_int *>(p)->data != &overlap_inits)
    return 1;
  overlap_deinits++;
  return 0;
}


/*
  test_sql_extension_bad_init: a valid descriptor whose init() fails. The
  plugin is refused, and its deinit() must not run.
*/
static const char *const bad_init_prefixes[]= { "TEST BADINIT", nullptr };

static st_mariadb_sql_extension bad_init_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_init_prefixes,
  nullptr,
  execute_noop,
  0
};

static int bad_init_inits= 0, bad_init_deinits= 0;

static int bad_init_init(void *)
{
  bad_init_inits++;
  return 1;
}

static int bad_init_deinit(void *)
{
  bad_init_deinits++;
  return 0;
}

static struct st_mysql_show_var main_status[]=
{
  { "test_sql_extension_overlap_inits", (char *) &overlap_inits, SHOW_INT },
  { "test_sql_extension_overlap_deinits", (char *) &overlap_deinits,
    SHOW_INT },
  { "test_sql_extension_bad_init_inits", (char *) &bad_init_inits,
    SHOW_INT },
  { "test_sql_extension_bad_init_deinits", (char *) &bad_init_deinits,
    SHOW_INT },
  { NULL, NULL, SHOW_UNDEF }
};


maria_declare_plugin(test_sql_extension)
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &main_descriptor,
  "test_sql_extension",
  "MariaDB plc",
  "SQL extension test: claims TEST SQLEXT",
  PLUGIN_LICENSE_GPL,
  NULL, main_deinit,
  0x0100,
  main_status, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &overlap_descriptor,
  "test_sql_extension_overlap",
  "MariaDB plc",
  "SQL extension test: claims TEST SQLEXT OVERLAP",
  PLUGIN_LICENSE_GPL,
  overlap_init, overlap_deinit,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_descriptor,
  "test_sql_extension_bad",
  "MariaDB plc",
  "SQL extension test: invalid descriptor",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &empty_descriptor,
  "test_sql_extension_empty",
  "MariaDB plc",
  "SQL extension test: neither prefixes nor recognize()",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_prefix_descriptor,
  "test_sql_extension_bad_prefix",
  "MariaDB plc",
  "SQL extension test: invalid prefix",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &mixed_descriptor,
  "test_sql_extension_mixed",
  "MariaDB plc",
  "SQL extension test: both prefixes and recognize()",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_priority_descriptor,
  "test_sql_extension_bad_priority",
  "MariaDB plc",
  "SQL extension test: a priority with prefixes",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_init_descriptor,
  "test_sql_extension_bad_init",
  "MariaDB plc",
  "SQL extension test: init() fails",
  PLUGIN_LICENSE_GPL,
  bad_init_init, bad_init_deinit,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &recognizer_descriptor,
  "test_sql_extension_recognizer",
  "MariaDB plc",
  "SQL extension test: recognizer",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &priority_descriptor,
  "test_sql_extension_priority",
  "MariaDB plc",
  "SQL extension test: recognizer with priority 10",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
