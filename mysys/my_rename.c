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
#include <my_dir.h>
#include "mysys_err.h"
#include "m_string.h"
#undef my_rename


#ifdef _WIN32

#define RENAME_MAX_RETRIES 50

/*
  On Windows, bad 3rd party programs (backup or anitivirus, or something else)
  can have file open with a sharing mode incompatible with renaming, i.e they
  won't use FILE_SHARE_DELETE when opening file.

  The following function will do a couple of retries, in case MoveFileEx returns
  ERROR_SHARING_VIOLATION.
*/
static BOOL win_rename_with_retries(const char *from, const char *to)
{
#ifndef DBUG_OFF
  FILE *fp = NULL;
  DBUG_EXECUTE_IF("rename_sharing_violation",
    {
    fp= fopen(from, "r");
    DBUG_ASSERT(fp);
    }
  );
#endif

  for (int retry= RENAME_MAX_RETRIES; retry--;)
  {
    BOOL ret= MoveFileEx(from, to,
                         MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);

    if (ret)
      return ret;

    DWORD last_error= GetLastError();
    if (last_error == ERROR_SHARING_VIOLATION ||
        last_error == ERROR_ACCESS_DENIED)
    {
#ifndef DBUG_OFF
       /*
        If error was injected in via DBUG_EXECUTE_IF, close the file
        that is causing ERROR_SHARING_VIOLATION, so that retry succeeds.
       */
        if (fp)
        {
          fclose(fp);
          fp= NULL;
        }
#endif

      Sleep(10);
    }
    else
      return ret;
  }
  return FALSE;
}
#endif

	/* On unix rename deletes to file if it exists */
int my_rename(const char *from, const char *to, myf MyFlags)
{
  int error = 0;
  DBUG_ENTER("my_rename");
  DBUG_PRINT("my",("from %s to %s MyFlags %lu", from, to, MyFlags));

#if defined(_WIN32)
  if (!win_rename_with_retries(from, to))
  {
    my_osmaperr(GetLastError());
#elif defined(HAVE_RENAME)
  if (rename(from,to))
  {
#else
  if (link(from, to) || unlink(from))
  {
#endif
    if (errno == ENOENT && !access(from, F_OK))
      my_errno= ENOTDIR;
    else
      my_errno= errno;
    error = -1;
    if (MyFlags & (MY_FAE+MY_WME))
      my_error(EE_LINK, MYF(ME_BELL),from,to,my_errno);
  }
  else if (MyFlags & MY_SYNC_DIR)
  {
#ifdef NEED_EXPLICIT_SYNC_DIR
    /* do only the needed amount of syncs: */
    char dir_from[FN_REFLEN], dir_to[FN_REFLEN];
    size_t dir_from_length, dir_to_length;
    dirname_part(dir_from, from, &dir_from_length);
    dirname_part(dir_to, to, &dir_to_length);
    if (my_sync_dir(dir_from, MyFlags) ||
        (strcmp(dir_from, dir_to) &&
         my_sync_dir(dir_to, MyFlags)))
      error= -1;
#endif
  }
  DBUG_RETURN(error);
} /* my_rename */
