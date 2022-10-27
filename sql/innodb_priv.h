/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef INNODB_PRIV_INCLUDED
#define INNODB_PRIV_INCLUDED

/** @file Declaring server-internal functions that are used by InnoDB. */

#include <sql_priv.h>
#include <strfunc.h>                            /* strconvert */

class THD;

int get_quote_char_for_identifier(THD *thd, const char *name, size_t length);
bool schema_table_store_record(THD *thd, TABLE *table);
void localtime_to_TIME(MYSQL_TIME *to, struct tm *from);

void sql_print_error(const char *format, ...);

#define thd_binlog_pos(X, Y, Z) mysql_bin_log_commit_pos(X, Z, Y)

#endif /* INNODB_PRIV_INCLUDED */
