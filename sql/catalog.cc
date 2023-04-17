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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_class.h"                          // killed_state
#include "mysql/services.h"                     // LEX_CSTRING
#include <m_string.h>                           // LEX_CSTRING
#include "sql_lex.h"                            // empty_lex_str
#include "catalog.h"

const LEX_CSTRING default_catalog_name= { STRING_WITH_LEN("def") };
SQL_CATALOG internal_default_catalog{&default_catalog_name, &empty_clex_str};
my_bool using_catalogs;

SQL_CATALOG::SQL_CATALOG(const LEX_CSTRING *name_arg,
                         const LEX_CSTRING *path_arg)
  :name(*name_arg), path(*path_arg)
{}


#ifdef NO_EMBEDDED_ACCESS_CHECKS
SQL_CATALOG *get_catalog(LEX_CSTRING *name)
{
  return &internal_default_catalog;
}

#else

SQL_CATALOG *get_catalog(LEX_CSTRING *name)
{
  DBUG_ENTER("get_catalog");
  DBUG_PRINT("enter", ("catalog: %.*s", name->length, name->str));
  if (name->length == internal_default_catalog.name.length &&
      !strncmp(name->str, internal_default_catalog.name.str,
               internal_default_catalog.name.length))
    DBUG_RETURN(&internal_default_catalog);
  DBUG_RETURN(0);
}
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
