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

#ifndef MYSQL_TRANSLATE_INCLUDED
#define MYSQL_TRANSLATE_INCLUDED

#include <my_global.h>
#include <stdio.h>

/* Dump output helpers, for MariaDB and MySQL sources alike. */

#define EX_EOM 4                        /* the exit code for out of memory */

/* Print the message and exit; defined by the host. */
[[noreturn]] void die(int error, const char *reason, ...)
  ATTRIBUTE_FORMAT(printf, 2, 3);

/*
  Print a single-quoted literal inside a version comment, with a comment
  terminator backslash-escaped.
*/
void dump_put_comment_safe_literal(FILE *out, const char *literal);

/* One "# " comment line; CR/LF are folded. */
void dump_emit_comment_warning(FILE *out, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 2, 3);

/*
  "CREATE USER" with OR REPLACE / IF NOT EXISTS, no trailing space: the account
  text, which starts with a space, follows.
*/
void dump_put_create_user(FILE *out, bool replace_into, bool ignore);

/* Translation of MySQL 5.7+ SHOW GRANTS / SHOW CREATE USER output. */

/*
  What the GRANTs of one account say about the global grant option. MySQL keeps
  the option per dynamic privilege, MariaDB per level.
*/
struct Mysql_grant_scan
{
  bool global_option;           /* ON *.* WITH GRANT OPTION, none mapped */
  bool mapped_without_option;   /* ON *.*, a mapped privilege, no option */

  /*
    True if the account could grant its mapped privileges, which MySQL did
    not allow.
  */
  bool drop_global_option() const
  { return global_option && mapped_without_option; }
};

/* Add a SHOW GRANTS row of the account to *scan. */
void scan_mysql_grant(const char *grant, Mysql_grant_scan *scan);

/*
  Returns true if handled (the caller must not print the GRANT).
  role_name, if not NULL, is the quoted role name replacing `role`@`host`.
  drop_global_option: drop WITH GRANT OPTION from an ON *.* grant, the
  Mysql_grant_scan::drop_global_option() of the account.
*/
bool translate_mysql_grant(const char *grant, FILE *out,
                           const char *role_name, bool drop_global_option);
void translate_mysql_revoke(const char *revoke, FILE *out,
                            const char *role_name);

/*
  Split a MySQL account list (mandatory_roles) as it was set: names quoted
  or not, the host optional ('%'). add() is called per account.
*/
void parse_mysql_account_list(const char *list,
                              void (*add)(const char *user, const char *host,
                                          void *arg),
                              void *arg);

/* user_str is the SHOW CREATE USER text after "CREATE USER", with its space. */
void translate_mysql_create_user(const char *user_str, FILE *out,
                                 bool replace_into, bool ignore);

#endif
