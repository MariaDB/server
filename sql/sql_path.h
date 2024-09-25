#ifndef SQL_PATH_INCLUDED
#define SQL_PATH_INCLUDED

/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2016, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mysqld.h"
#include "sql_array.h"


/* forward declarations */
class Database_qualified_name;


class Sql_path
{
private:
  Dynamic_array<LEX_CSTRING> db_list;

public:
  Sql_path();
  void append_db(char *in);
  void strtok_db(char *in);
  bool find_db_unqualified(THD *thd, const LEX_CSTRING &name,
                           const Sp_handler *sph,
                           Lex_ident_db_normalized *dbn_out,
                           sp_name **spname_out);
  bool find_db_qualified(THD *thd,
                         sp_name *name,
                         const Sp_handler **sph,
                         Database_qualified_name *pkgname);
  void free_db_list();
};

#endif /* SQL_PATH_INCLUDED */
