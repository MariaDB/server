/* Copyright (c) 2000, 2001, 2005-2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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
#include <errno.h>
#include <my_sys.h>
#if defined(_WIN32)
#include <share.h>
#endif

	/*
	** Create a new file
	** Arguments:
	** Path-name of file
	** Read | write on file (umask value)
	** Read & Write on open file
	** Special flags
	*/


File my_create(const char *FileName, int CreateFlags, int access_flags,
	       myf MyFlags)
{
  int fd;
  DBUG_ENTER("my_create");
  DBUG_PRINT("my",("Name: '%s' CreateFlags: %d  AccessFlags: %d  MyFlags: %lu",
		   FileName, CreateFlags, access_flags, MyFlags));
#if defined(_WIN32)
  fd= my_win_open(FileName, access_flags | O_CREAT);
#else
  fd= open((char *) FileName, access_flags | O_CREAT | O_CLOEXEC,
	    CreateFlags ? CreateFlags : my_umask);
#endif

  if ((MyFlags & MY_SYNC_DIR) && (fd >=0) &&
      my_sync_dir_by_file(FileName, MyFlags))
  {
    my_close(fd, MyFlags);
    fd= -1;
  }

  fd= my_register_filename(fd, FileName, FILE_BY_CREATE,
                           EE_CANTCREATEFILE, MyFlags);
  DBUG_RETURN(fd);
} /* my_create */
