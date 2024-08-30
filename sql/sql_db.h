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

#ifndef SQL_DB_INCLUDED
#define SQL_DB_INCLUDED

#include "hash.h"                               /* HASH */

class THD;

int mysql_create_db(THD *thd, const Lex_ident_db &db, DDL_options_st options,
                    const Schema_specification_st *create);
bool mysql_alter_db(THD *thd, const Lex_ident_db &db,
                    const Schema_specification_st *create);
bool mysql_rm_db(THD *thd, const Lex_ident_db &db, bool if_exists);
bool mysql_upgrade_db(THD *thd, const Lex_ident_db &old_db);
uint mysql_change_db(THD *thd, const LEX_CSTRING *new_db_name,
                     bool force_switch);

bool mysql_opt_change_db(THD *thd,
                         const LEX_CSTRING *new_db_name,
                         LEX_STRING *saved_db_name,
                         bool force_switch,
                         bool *cur_db_changed);
bool my_dboptions_cache_init(void);
void my_dboptions_cache_free(void);
bool check_db_dir_existence(const char *db_name);
bool load_db_opt(THD *thd, const char *path, Schema_specification_st *create);
bool load_db_opt_by_name(THD *thd, const char *db_name,
                         Schema_specification_st *db_create_info);
CHARSET_INFO *get_default_db_collation(THD *thd, const char *db_name);
bool my_dbopt_init(void);
void my_dbopt_cleanup(void);

void drop_database_objects(THD *thd, const LEX_CSTRING *path,
                           const LEX_CSTRING *db,
                           bool rm_mysql_schema);
my_bool rm_dir_w_symlink(const char *org_path, my_bool send_error);
#define MY_DB_OPT_FILE "db.opt"

#endif /* SQL_DB_INCLUDED */
