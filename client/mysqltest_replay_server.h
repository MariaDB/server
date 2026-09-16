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

#ifndef MYSQLTEST_REPLAY_SERVER_INCLUDED
#define MYSQLTEST_REPLAY_SERVER_INCLUDED

/*
  Replay-server mode of mysqltest, the client half of mtr --replay-server.

  When mariadb-test-run.pl starts a second ("replay") server it passes its
  socket in REPLAY_SERVER_SOCKET.  mysqltest then makes the test server record
  the optimizer context of every EXPLAIN, replays that context on the replay
  server and puts the replay server's EXPLAIN output into the test result in
  place of the test server's own.  Everything that implements this lives in
  mysqltest_replay_server.cc; mysqltest.cc reaches it through the entry points
  below.

  Include after client_priv.h - MYSQL, DYNAMIC_STRING & co. are assumed known.
*/

#include <my_attribute.h>

/* -- Entry points implemented in mysqltest_replay_server.cc -------------- */

/*
  Set up replay-server mode from the environment (REPLAY_SERVER_SOCKET and
  friends).  A no-op when REPLAY_SERVER_SOCKET is not set, which is the
  ordinary case.  `result_file_name` is --result-file, or NULL; it gives the
  optimizer_trace dumps their names.
*/
void replay_init(const char *result_file_name);

/* Close the replay connection and release everything replay_init() set up */
void replay_free(void);

/*
  Argument handler of the "disable_replay <scope> <reason>" command.
  `arg` .. `end` is the argument text.
  Returns NULL on success, or the message to die() with on a syntax error.
*/
const char *replay_do_disable(const char *arg, const char *end);

/*
  Pre-query hook: decide whether this query is to be replayed and, if it is,
  make the test server record its optimizer context.  Called once per query -
  also for queries that are not replayed, so that one-shot flags such as
  "disable_replay next_query" are consumed exactly once.

  `complete_query` says whether the query is both sent and reaped by this
  call; only such a query can have its result set replaced.

  Returns TRUE when the hook is active for this query, that is, when its first
  result set is to be replaced with the replay server's output.
*/
my_bool replay_hook_pre_query(MYSQL *mysql, my_bool complete_query,
                              const char *query, size_t query_len);

/*
  Result hook: replace the EXPLAIN result of the test server with the one the
  replay server produces from the recorded optimizer context.  Consumes *res.
*/
void replay_hook_result(MYSQL *mysql, MYSQL_RES **res, MYSQL_FIELD *fields,
                        uint num_fields, const char *query, size_t query_len,
                        DYNAMIC_STRING *ds);

/*
  Undo on the test server what replay_hook_pre_query() set up.  Called by
  replay_hook_result(), and directly by mysqltest.cc when a query armed by
  replay_hook_pre_query() never produced a result set.
*/
void replay_undo_test_server_setup(MYSQL *mysql);

/* -- Shared functions defined in mysqltest.cc ---------------------------- */

void verbose_msg(const char *fmt, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
void append_field(DYNAMIC_STRING *ds, uint col_idx, MYSQL_FIELD *field,
                  char *val, size_t len, my_bool is_null);
void append_table_headings(DYNAMIC_STRING *ds, MYSQL_FIELD *field,
                           uint num_fields);
void append_result(DYNAMIC_STRING *ds, MYSQL_RES *res);
int append_warnings(DYNAMIC_STRING *ds, MYSQL *mysql);

/* Current settings of the running mysqltest */
extern CHARSET_INFO *charset_info;   /* the charset input is parsed in */
extern my_bool disable_warnings;
extern my_bool display_result_vertically;

/* TRUE for "EXPLAIN ..." queries that the replay server can handle */
my_bool is_explain_query(const char *query, size_t query_len);

/* Print the current test-file location, each line prefixed with `prefix` */
void print_test_location(FILE *f, const char *prefix);

#endif /* MYSQLTEST_REPLAY_SERVER_INCLUDED */
