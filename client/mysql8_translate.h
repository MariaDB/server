/*
   Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MYSQL8_TRANSLATE_INCLUDED
#define MYSQL8_TRANSLATE_INCLUDED

#include <my_global.h>
#include <stdio.h>

/* Dump output helpers, for MariaDB and MySQL sources alike. */

/* Fatal error (out of memory, formatting failure); defined by the host. */
[[noreturn]] void dump_fatal(const char *msg);

/*
  Print a quoted literal inside a version comment, as CONCAT('..*', '/..')
  if it contains a comment terminator.
*/
void dump_put_comment_safe_literal(FILE *out, const char *literal);

/* One "# " comment line; CR/LF are folded. */
void dump_emit_comment_warning(FILE *out, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 2, 3);

/*
  Version comment: prefix, formatted body, comment end and ';'.
  A body with a comment terminator goes through EXECUTE IMMEDIATE CONCAT().
*/
void dump_emit_versioned_wrap(FILE *out, const char *prefix,
                              const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 3, 4);

/* Translation of MySQL 5.7+ SHOW GRANTS / SHOW CREATE USER output. */

/*
  Returns true if handled (the caller must not print the GRANT).
  role_name, if not NULL, is the quoted role name replacing `role`@`host`.
*/
bool translate_mysql8_grant(const char *grant, FILE *out,
                            const char *role_name);
void translate_mysql8_revoke(const char *revoke, FILE *out);

/* user_str is the SHOW CREATE USER text after "CREATE USER", with its space. */
void translate_mysql8_create_user(const char *user_str, FILE *out,
                                  bool replace_into, bool ignore);

#endif
