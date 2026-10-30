/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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

#include "mysys_priv.h"
#include "mysys_err.h"
#include <my_sys.h>

#ifdef _WIN32
#include <direct.h> /* rmdir */
#endif

CREATE_NOSYMLINK_FUNCTION(
  unlink_nosymlinks(const char *pathname),
  unlinkat(dfd, filename, 0),
  unlink(pathname)
);

int my_delete(const char *name, myf MyFlags)
{
  int err;
  DBUG_ENTER("my_delete");
  DBUG_PRINT("my",("name %s MyFlags %lu", name, MyFlags));

#ifdef _WIN32
  err = my_win_unlink(name, MyFlags);
#else
  if (MyFlags & MY_NOSYMLINKS)
    err= unlink_nosymlinks(name);
  else
    err= unlink(name);
#endif

  if ((MyFlags & MY_IGNORE_ENOENT) && errno == ENOENT)
    DBUG_RETURN(0);

  if (err)
  {
    my_errno= errno;
    if (MyFlags & (MY_FAE+MY_WME))
      my_error(EE_DELETE, MYF(ME_BELL), name, errno);
  }
  else if ((MyFlags & MY_SYNC_DIR) && my_sync_dir_by_file(name, MyFlags))
    err= -1;
  DBUG_RETURN(err);
} /* my_delete */


/*
   Remove directory recursively.
*/
int my_rmtree(const char *dir, myf MyFlags)
{
  char path[FN_REFLEN];
  char sep[] = { FN_LIBCHAR, 0 };
  int err = 0;
  size_t i;

  MY_DIR *dir_info = my_dir(dir, MYF(MY_DONT_SORT | MY_WANT_STAT));
  if (!dir_info)
    return 1;

  for (i = 0; i < dir_info->number_of_files; i++)
  {
    FILEINFO *file = dir_info->dir_entry + i;
    /* Skip "." and ".." */
    if (!strcmp(file->name, ".") || !strcmp(file->name, ".."))
      continue;

    strxnmov(path, sizeof(path), dir, sep, file->name, NULL);

    if (!MY_S_ISDIR(file->mystat->st_mode))
    {
      err = my_delete(path, MyFlags);
#ifdef _WIN32
      /*
        On Windows, check and possible reset readonly attribute.
        my_delete(), or DeleteFile does not remove theses files.
      */
      if (err)
      {
        DWORD attr = GetFileAttributes(path);
        if (attr != INVALID_FILE_ATTRIBUTES &&
          (attr & FILE_ATTRIBUTE_READONLY))
        {
          SetFileAttributes(path, attr &~FILE_ATTRIBUTE_READONLY);
          err = my_delete(path, MyFlags);
        }
      }
#endif
    }
    else
      err = my_rmtree(path, MyFlags);

    if (err)
      break;
  }

  my_dirend(dir_info);

  if (!err)
    err = rmdir(dir);

  return err;
}


