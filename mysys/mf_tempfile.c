/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.

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
#include <m_string.h>
#include "my_static.h"
#include "mysys_err.h"
#include <errno.h>
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifdef HAVE_MKOSTEMP
#define mkstemp(A) mkostemp(A, O_CLOEXEC)
#endif

/*
  @brief
  Create a temporary file with unique name in a given directory

  @details
  create_temp_file
    to             pointer to buffer where temporary filename will be stored
    dir            directory where to create the file
    prefix         prefix the filename with this
    mode           Flags to use for my_create/my_open
    MyFlags        Magic flags

  @return
    File descriptor of opened file if success
    -1 and sets errno if fails.

  @note
    The behaviour of this function differs a lot between
    implementation, it's main use is to generate a file with
    a name that does not already exist.

    When passing MY_TEMPORARY flag in MyFlags the file is automatically deleted

    "mode" bits that always must be used for newly created files with
    unique file names (O_EXCL | O_TRUNC | O_CREAT | O_RDWR) are added
    automatically, and shouldn't be specified by the caller.

    The implementation using mkstemp should be considered the
    reference implementation when adding a new or modifying an
    existing one

*/

File create_temp_file(char *to, const char *dir, const char *prefix,
		      int mode, myf MyFlags)
{
  File file= -1;

  DBUG_ENTER("create_temp_file");
  DBUG_PRINT("enter", ("dir: %s, prefix: %s", dir ? dir : "(null)", prefix));
  DBUG_ASSERT((mode & (O_EXCL | O_TRUNC | O_CREAT | O_RDWR)) == 0);

  mode|= O_TRUNC | O_CREAT | O_RDWR; /* not O_EXCL, see Windows code below */

#ifdef _WIN32
  {
    TCHAR path_buf[MAX_PATH-14];
    /*
      Use GetTempPath to determine path for temporary files.
      This is because the documentation for GetTempFileName
      has the following to say about this parameter:
      "If this parameter is NULL, the function fails."
    */
    if (!dir)
    {
      if(GetTempPath(sizeof(path_buf), path_buf) > 0)
        dir = path_buf;
    }
    /*
      Use GetTempFileName to generate a unique filename, create
      the file and release it's handle
       - uses up to the first three letters from prefix
    */
    if (GetTempFileName(dir, prefix, 0, to) == 0)
      DBUG_RETURN(-1);

    DBUG_PRINT("info", ("name: %s", to));

    if (MyFlags & MY_TEMPORARY)
      mode|= O_SHORT_LIVED | O_TEMPORARY;

    /*
      Open the file without O_EXCL flag
      since the file has already been created by GetTempFileName
    */
    if ((file= my_open(to, mode, MyFlags)) < 0)
    {
      /* Open failed, remove the file created by GetTempFileName */
      int tmp= my_errno;
      (void) my_delete(to, MYF(0));
      my_errno= tmp;
    }
  }
#elif defined(HAVE_MKSTEMP)
  if (!dir && ! (dir =getenv("TMPDIR")))
    dir= DEFAULT_TMPDIR;
#ifdef O_TMPFILE
  {
    static int O_TMPFILE_works= 1;

    if ((MyFlags & MY_TEMPORARY) && O_TMPFILE_works)
    {
      /* explictly don't use O_EXCL here has it has a different
         meaning with O_TMPFILE
      */
      if ((file= open(dir, (mode & ~O_CREAT) | O_TMPFILE | O_CLOEXEC,
                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) >= 0)
      {
        my_snprintf(to, FN_REFLEN, "%s/#sql/fd=%d", dir, file);
        file=my_register_filename(file, to, FILE_BY_O_TMPFILE,
                                  EE_CANTCREATEFILE, MyFlags);
      }
      else if (errno == EOPNOTSUPP || errno == EINVAL)
      {
        my_printf_error(EE_CANTCREATEFILE, "O_TMPFILE is not supported on %s "
                        "(disabling future attempts)",
                        MYF(ME_NOTE | ME_ERROR_LOG_ONLY), dir);
        O_TMPFILE_works= 0;
      }
    }
  }
  if (file == -1)
#endif /* O_TMPFILE */
  {
    char prefix_buff[30];
    uint pfx_len;
    File org_file;

    pfx_len= (uint) (strmov(strnmov(prefix_buff,
				    prefix ? prefix : "tmp.",
				    sizeof(prefix_buff)-7),"XXXXXX") -
		     prefix_buff);
    if (strlen(dir)+ pfx_len > FN_REFLEN-2)
    {
      errno=my_errno= ENAMETOOLONG;
      DBUG_RETURN(file);
    }
    strmov(convert_dirname(to,dir,NullS),prefix_buff);
    org_file=mkstemp(to);
    if (org_file >= 0 && (MyFlags & MY_TEMPORARY))
      (void) my_delete(to, MYF(MY_WME));
    file=my_register_filename(org_file, to, FILE_BY_MKSTEMP,
			      EE_CANTCREATEFILE, MyFlags);
    /* If we didn't manage to register the name, remove the temp file */
    if (org_file >= 0 && file < 0)
    {
      int tmp=my_errno;
      close(org_file);
      (void) my_delete(to, MYF(MY_WME));
      my_errno=tmp;
    }
  }
#else
#error No implementation found for create_temp_file
#endif
  if (file >= 0)
    statistic_increment(my_tmp_file_created,&THR_LOCK_open);
  DBUG_RETURN(file);
}
