/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates

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

/*
  deletes a table
*/

#include "fulltext.h"

#ifndef HAVE_PSI_INTERFACE
#define PSI_file_key int
#define mi_key_file_kfile 0
#define mi_key_file_dfile 0
#endif

int mi_delete_table(const char *name)
{
  int error= 0;
  DBUG_ENTER("mi_delete_table");

#ifdef EXTRA_DEBUG
  check_table_is_closed(name,"delete");
#endif

  if (mysql_file_delete_with_symlink(mi_key_file_kfile, name, MI_NAME_IEXT,
                                     MYF(MY_WME)))
    error= my_errno;
  if (mysql_file_delete_with_symlink(mi_key_file_dfile, name, MI_NAME_DEXT,
                                     MYF(MY_WME)))
    error= my_errno;

  // optionally present:
  mysql_file_delete_with_symlink(mi_key_file_dfile, name, ".OLD", MYF(0));
  mysql_file_delete_with_symlink(mi_key_file_dfile, name, ".TMD", MYF(0));

  DBUG_RETURN(error);
}
