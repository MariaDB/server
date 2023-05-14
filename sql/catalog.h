#ifndef CATALOG_INCLUDED
#define CATALOG_INCLUDED

/* Copyright (c) 2023, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

#define MY_CATALOG_OPT_FILE "catalog.opt"

#include "sql_hset.h"
#include "sql_string.h"
#include "privilege.h"

struct Schema_specification_st;

class SQL_CATALOG
{
public:
  SQL_CATALOG *next;                            // For drop catalog
  SQL_CATALOG(const LEX_CSTRING *name, const LEX_CSTRING *path);
  const LEX_CSTRING name;
  const LEX_CSTRING path;             // Directory path, including '/'
  LEX_CSTRING comment;                // Comment from 'catalog.opt'
  CHARSET_INFO *cs;
  privilege_t acl;                    // acl's allowed for this catalog
  ulong event_scheduler;              // Default for event scheduler
  bool initialized;                   // If object has been fully initalized
  bool deleted;                       // If object has been deleted

  /* Init catalog based on global variables */
  void initialize_from_env();
  /* Init catalogs when server is up and tables can be read */
  bool late_init();
};

extern my_bool using_catalogs;
extern SQL_CATALOG *get_catalog(const LEX_CSTRING *name, bool initialize);
extern SQL_CATALOG *get_catalog_with_error(const THD *thd,
                                           const LEX_CSTRING *name,
                                           bool initialize);
extern const LEX_CSTRING default_catalog_name;
extern SQL_CATALOG internal_default_catalog;
inline SQL_CATALOG *default_catalog()
{
  return &internal_default_catalog;
}

extern bool check_if_using_catalogs();
extern bool check_catalog_access(THD *thd, const LEX_CSTRING *name);
extern bool init_catalogs(const char *datadir);
extern bool late_init_all_catalogs();
extern void free_catalogs();
extern bool mariadb_change_catalog(THD *thd, const LEX_CSTRING *catalog_name);
extern int maria_create_catalog(THD *thd, const LEX_CSTRING *name,
                                DDL_options_st options,
                                const Schema_specification_st *create_info);
extern bool maria_rm_catalog(THD *thd, const LEX_CSTRING *db, bool if_exists);
extern bool maria_alter_catalog(THD *thd, SQL_CATALOG *catalog,
                                const Schema_specification_st *create_info);

extern mysql_mutex_t LOCK_catalogs;
extern Hash_set <SQL_CATALOG> catalog_hash;

#endif /* CATALOG_INCLUDED */
