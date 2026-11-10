#ifndef SQL_EXTENSION_INCLUDED
#define SQL_EXTENSION_INCLUDED
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

/**
  @file
  SQL extension framework: registry of MariaDB_SQL_EXTENSION_PLUGIN plugins.

  The registry is consulted only after a statement failed to parse. With no
  extension plugin installed that costs one atomic load.
*/

class THD;
class Parser_state;

/**
  Offer a statement that failed to parse to the extension plugins.

  Called after parse_sql() failed, from exactly three places: mysql_parse()
  (client statements), Prepared_statement::prepare() (PREPARE, EXECUTE
  IMMEDIATE, COM_STMT_PREPARE) and execute_server_code() (SQL that plugins
  run through the SQL service, and through sql_extension_forward_sql()). Proceeds only on ER_PARSE_ERROR (the code
  of every syntax error of the parser; ER_SYNTAX_ERROR is only a text in
  it) that is not fatal, outside replication appliers (asynchronous and
  Galera), stored functions, triggers, the parsing of a stored program
  body and bootstrap.

  On a claim the parse error and its conditions are cleared, the LEX is
  restarted and holds SQLCOM_SQL_EXTENSION, and for a claim that ends
  before the end of a multi-statement packet, found_semicolon points at
  the rest.

  When it returns false, the parse error may have been replaced by one
  that says why the statement was not claimed: ER_SQL_EXTENSION_NESTED
  (claimed while another extension statement is being executed on this
  thread), ER_SQL_EXTENSION_INVALID_RESULT (a recognizer claimed past the
  end of the statement), ER_OUT_OF_RESOURCES (the plugin could not be
  pinned, or the command could not be allocated), or ER_PARSE_ERROR near
  text that cannot follow the claim. Only a statement no plugin claims
  keeps its parse error untouched.

  @return true if claimed: the caller executes the statement as if parsed
*/
bool sql_extension_dispatch(THD *thd, Parser_state *parser_state);

/**
  A number unique to the extension statement executing on thd among all
  those of the server, or 0 if none is executing. Lets the as-client
  connection of the SQL service refuse use by a later statement
  (sql_extension_sql_as_client() is declared in sql_class.h).
*/
ulonglong sql_extension_statement_serial(const THD *thd);

/**
  Set up the registry. Called once, before plugins are initialized.
  @return true if out of memory: start-up aborts, and sql_extension_free()
          frees what was set up
*/
bool sql_extension_init();

/** Free the registry. Called once, after plugins are shut down. */
void sql_extension_free();

/** Per-type plugin hooks (plugin_type_initialize[] in sql_plugin.cc) */
int initialize_sql_extension_plugin(void *plugin);
int finalize_sql_extension_plugin(void *plugin);

#endif /* SQL_EXTENSION_INCLUDED */
