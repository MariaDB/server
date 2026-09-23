/* Copyright (C) MariaDB Corporation Ab

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include <my_global.h>
#include <mysqld.h>
#include <sql_parse.h>
#include "filechk.h"

/*
  @return true if the current user may access path.
*/
bool connect_can_access_file(const char *path)
{
  char real_path[FN_REFLEN];

  if (!path)
    return false; /* callers must check for NULL before calling */

  if (check_global_access(current_thd, FILE_ACL, true))
    return false;

  /* MY_SAFE_PATH returns NULL on overflow; fail closed rather than validate a truncated path */
  if (!fn_format(real_path, path, mysql_real_data_home, "",
                 MY_RELATIVE_PATH | MY_UNPACK_FILENAME | MY_RETURN_REAL_PATH | MY_SAFE_PATH))
    return false;

  return is_secure_file_path(real_path);
}
