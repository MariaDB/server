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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  deletes a table
*/

#include "fulltext.h"

#ifndef HAVE_PSI_INTERFACE
#define PSI_file_key int
#define mi_key_file_kfile 0
#define mi_key_file_dfile 0
#endif

static int delete_one_file(const char *name, const char *ext,
                           PSI_file_key pskey __attribute__((unused)),
                           myf flags)
{
  char from[FN_REFLEN];
  DBUG_ENTER("delete_one_file");
  fn_format(from,name, "", ext, MY_UNPACK_FILENAME | MY_APPEND_EXT);
  if (my_is_symlink(from) && (*myisam_test_invalid_symlink)(from))
  {
    /*
      Symlink is pointing to file in data directory.
      Remove symlink, keep file.
    */
    if (mysql_file_delete(pskey, from, flags))
      DBUG_RETURN(my_errno);
  }
  else
  {
    if (mysql_file_delete_with_symlink(pskey, from, flags))
      DBUG_RETURN(my_errno);
  }
  DBUG_RETURN(0);
}

int mi_delete_table(const char *name)
{
  int res;
  DBUG_ENTER("mi_delete_table");

#ifdef EXTRA_DEBUG
  check_table_is_closed(name,"delete");
#endif

  if ((res= delete_one_file(name, MI_NAME_IEXT, mi_key_file_kfile, MYF(MY_WME))))
    DBUG_RETURN(res);
  if ((res= delete_one_file(name, MI_NAME_DEXT, mi_key_file_dfile, MYF(MY_WME))))
    DBUG_RETURN(res);

  // optionally present:
  delete_one_file(name, ".OLD", mi_key_file_dfile, MYF(0));
  delete_one_file(name, ".TMD", mi_key_file_dfile, MYF(0));

  DBUG_RETURN(0);
}

