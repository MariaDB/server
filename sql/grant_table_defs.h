/*
   Copyright (c) 2026, MariaDB

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

#ifndef GRANT_TABLE_DEFS_INCLUDED
#define GRANT_TABLE_DEFS_INCLUDED

#include "table.h"                              // TABLE_FIELD_DEF

/*
  Expected structure of the grant tables, see grant_table_defs.cc.
  Keep the definitions there in sync with scripts/mysql_system_tables.sql.
*/
extern const TABLE_FIELD_DEF mysql_db_table_def;
extern const TABLE_FIELD_DEF mysql_roles_mapping_table_def;
extern const TABLE_FIELD_DEF mysql_proxies_priv_table_def;
extern const TABLE_FIELD_DEF mysql_tables_priv_table_def;
extern const TABLE_FIELD_DEF mysql_columns_priv_table_def;
extern const TABLE_FIELD_DEF mysql_procs_priv_table_def;
extern const TABLE_FIELD_DEF mysql_global_priv_table_def;

#endif /* GRANT_TABLE_DEFS_INCLUDED */
