#ifndef MARIADB_PLUGIN_SQL_EXTENSION_INCLUDED
#define MARIADB_PLUGIN_SQL_EXTENSION_INCLUDED
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

  SQL Extension Plugin API.

  An SQL extension plugin handles statements that the server's own parser
  rejects. The server consults installed extension plugins only after a
  statement failed to parse with a syntax error, so a plugin can add new
  statements but can never change the meaning of valid SQL, and statements
  that parse cost nothing extra.

  Statements reach a plugin from clients: sent directly, prepared and
  executed, or run as dynamic SQL in a stored procedure or event. A prepared
  extension statement has no parameters: a ? in it is text for the
  plugin, and EXECUTE ... USING is refused. They never come
  from the body of a stored program, view, trigger or event, nor from
  replication, which carries the SQL a handler ran as the client rather
  than the extension statement. They also reach it from the SQL another
  plugin runs through the plain SQL service (service_sql.h): such SQL
  runs with no grants checked, and under LOCK TABLES on a connection of
  its own, so sql_extension_check_access() then grants everything but
  what the built-in rules deny to every user (information_schema,
  performance_schema).

  execute() runs on the client's connection, in the client's thread,
  while the statement executes: the plugin stays locked until it returns,
  so UNINSTALL PLUGIN unloads it only afterwards. The SQL a handler runs
  on behalf of the statement goes through the SQL extension service
  (service_sql_extension.h), as the client.

  plugin/test_sql_extension is a complete example, exercised by the
  mysql-test suite sql_extension.
*/

/**
  @defgroup sql_extension_plugin SQL Extension Plugin
  @ingroup plugin_types
  API for SQL extension plugins
  (@ref MariaDB_SQL_EXTENSION_INTERFACE_VERSION).
  @{
*/

#include <mysql/plugin.h>
#include <mysql/service_sql_extension.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MariaDB_SQL_EXTENSION_INTERFACE_VERSION 0x0100

/**
  Descriptor of an SQL extension plugin: the info pointer of a plugin
  declared with type MariaDB_SQL_EXTENSION_PLUGIN.

  A plugin claims statements in one of two ways, never both:

  - Prefixes (recognize == NULL): every statement that starts with one of
    the prefixes is the plugin's. The statement ends, as an ordinary one
    does, at the first ';' outside quoted strings, quoted identifiers and
    comments, read with the session's sql_mode (ANSI_QUOTES,
    NO_BACKSLASH_ESCAPES); executable comments count as comments. The
    plugin gets the text before that ';', and the statements after it run
    as for a recognizer's claim (see recognize). Its syntax must therefore
    keep any ';' that does not end the statement inside quotes.
  - A recognizer (prefixes == NULL): the callback decides and reports how
    many bytes it claims. It is asked about every statement that failed to
    parse and that no prefix claimed, so it must reject what is not its
    own quickly, typically by its first keyword.

  A statement that starts with a plugin's prefix is that plugin's before
  any recognizer is asked. Recognizers are asked in order of priority,
  highest first, and the first to claim wins; between equal priorities
  the plugin name decides, so the order never depends on when a plugin
  was installed.

  Prefixes are lists of keywords separated by whitespace, such as
  "CREATE REST" or "CREATE OR REPLACE REST". A keyword starts with an ASCII
  letter or '_' and continues with ASCII letters, digits, '_' or '$', 64
  bytes at most. Matching is case-insensitive and keyword-based:
  whitespace and comments may appear before and between the keywords,
  executable comments included (the matcher skips them whole, whereas the
  server parser reads their contents), and "CREATE REST" does not match
  "CREATE RESTS". A prefix that is a prefix of another plugin's prefix (in
  either direction) is refused at INSTALL time.
*/
struct st_mariadb_sql_extension
{
  /** MariaDB_SQL_EXTENSION_INTERFACE_VERSION */
  int interface_version;

  /**
    NULL-terminated list of keyword prefixes the plugin claims. NULL (or
    empty) when recognize is set.
  */
  const char *const *prefixes;

  /**
    Recognizer, NULL when prefixes are set. Must be a pure function: no
    THD access, no side effects, and fast, since it runs on the
    failed-parse path of other users' statements. It runs with the
    registry read-locked: it must not run SQL, nor anything that may
    install or uninstall a plugin, or it deadlocks.

    @param query   the text from the start of the statement to the end of
                   the packet, in the client character set
    @param length  its length in bytes

    @return the number of bytes of query the plugin claims, or 0 if the
            statement is not the plugin's. A value above length is a
            plugin bug: the statement fails with an error that names the
            plugin, and no other recognizer is asked. A claim shorter
            than the text must be followed by whitespace and ';' only, or
            by ';' and further statements in a packet the client sent as
            several statements; otherwise the statement fails with a
            syntax error near the text that follows the claim. The
            statements after the claim run normally.
  */
  size_t (*recognize)(const char *query, size_t length);

  /**
    Required. Handles a claimed statement completely: it sends one or more
    results (result sets, OKs, results of forwarded SQL) through the SQL
    extension service, or an error through the my_print_error service, or
    results followed by an error (see service_sql_extension.h). If it sends
    nothing and returns 0, the server sends OK.

    @param thd     the client connection
    @param query   the claimed statement text, in the client character set
    @param length  its length in bytes

    @return 0 on success, non-zero on failure. A failure without an error
            raised is reported as ER_SQL_EXTENSION_FAILED. An OK or end of
            rows set on the connection by other means than the service is
            refused as an invalid result.
  */
  int (*execute)(MYSQL_THD thd, const char *query, size_t length);

  /**
    Order among recognizers: higher first; the plugin name breaks ties.
    0 when left out. Must be 0 with prefixes, which cannot overlap.
  */
  int priority;
};

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MARIADB_PLUGIN_SQL_EXTENSION_INCLUDED */
