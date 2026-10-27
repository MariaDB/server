/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_RENAME_INCLUDED
#define SQL_RENAME_INCLUDED

class THD;
struct TABLE_LIST;

/**
   Parameters for do_rename
*/

struct rename_param
{
  Lex_ident_table old_alias, new_alias;
  LEX_CUSTRING old_version;
  handlerton *from_table_hton;
  int rename_flags; /* FN_FROM_IS_TMP, FN_TO_IS_TMP */
  rename_param():
   from_table_hton(NULL),
   rename_flags(0) {}
};

bool mysql_rename_tables(THD *thd, TABLE_LIST *table_list, bool silent,
                         bool if_exists);
bool do_rename(THD *thd, const rename_param *param, DDL_LOG_STATE *ddl_log_state,
               TABLE_LIST *ren_table, const Lex_ident_db *new_db,
               bool skip_error, bool *force_if_exists);

#endif /* SQL_RENAME_INCLUDED */
