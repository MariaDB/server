/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

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
#include <m_string.h>
#include <errno.h>
#include "my_atomic.h"

CREATE_NOSYMLINK_FUNCTION(
  open_nosymlinks(const char *pathname, int flags, int mode),
  openat(dfd, filename, O_NOFOLLOW | flags, mode),
  open(pathname, O_NOFOLLOW | flags, mode)
);

/*
  Open a file

  SYNOPSIS
    my_open()
      FileName	Fully qualified file name
      Flags	Read | write 
      MyFlags	Special flags

  RETURN VALUE
    File descriptor
*/

File my_open(const char *FileName, int Flags, myf MyFlags)
				/* Path-name of file */
				/* Read | write .. */
				/* Special flags */
{
  File fd;
  DBUG_ENTER("my_open");
  DBUG_PRINT("my",("Name: '%s'  Flags: %d  MyFlags: %lu",
		   FileName, Flags, MyFlags));
  if (!(MyFlags & (MY_WME | MY_FAE | MY_FFNF)))
    MyFlags|= my_global_flags;
#if defined(_WIN32)
  fd= my_win_open(FileName, Flags);
#else
  if (MyFlags & MY_NOSYMLINKS)
    fd = open_nosymlinks(FileName, Flags | O_CLOEXEC, my_umask);
  else
    fd = open(FileName, Flags | O_CLOEXEC, my_umask);
#endif

  fd= my_register_filename(fd, FileName, FILE_BY_OPEN,
			   EE_FILENOTFOUND, MyFlags);
  DBUG_RETURN(fd);
} /* my_open */


/*
  Close a file

  SYNOPSIS
    my_close()
      fd	File sescriptor
      myf	Special Flags

*/

int my_close(File fd, myf MyFlags)
{
  int err;
  char *name= NULL;
  DBUG_ENTER("my_close");
  DBUG_PRINT("my",("fd: %d  MyFlags: %lu",fd, MyFlags));
  if (!(MyFlags & (MY_WME | MY_FAE)))
    MyFlags|= my_global_flags;

  if ((uint) fd < my_file_limit && my_file_info[fd].type != UNOPEN)
  {
    name= my_file_info[fd].name;
    my_file_info[fd].name= NULL;
    my_file_info[fd].type= UNOPEN;
  }
#ifndef _WIN32
  err= close(fd);
#else
  err= my_win_close(fd);
#endif
  if (err)
  {
    DBUG_PRINT("error",("Got error %d on close",err));
    my_errno=errno;
    if (MyFlags & (MY_FAE | MY_WME))
      my_error(EE_BADCLOSE, MYF(ME_BELL | (MyFlags & (ME_NOTE | ME_ERROR_LOG))),
               name,errno);
  }
  if (name)
  {
    my_free(name);
  }
  my_atomic_add32_explicit(&my_file_opened, -1, MY_MEMORY_ORDER_RELAXED);
  DBUG_RETURN(err);
} /* my_close */


/*
  Register file in my_file_info[]
   
  SYNOPSIS
    my_register_filename()
    fd			   File number opened, -1 if error on open
    FileName		   File name
    type_file_type	   How file was created
    error_message_number   Error message number if caller got error (fd == -1)
    MyFlags		   Flags for my_close()

  RETURN
    -1   error
     #   Filenumber

*/

File my_register_filename(File fd, const char *FileName, enum file_type
			  type_of_file, uint error_message_number, myf MyFlags)
{
  DBUG_ENTER("my_register_filename");
  if ((int) fd >= MY_FILE_MIN)
  {
    my_atomic_add32_explicit(&my_file_opened, 1, MY_MEMORY_ORDER_RELAXED);
    if ((uint) fd >= my_file_limit || (MyFlags & MY_NO_REGISTER))
      DBUG_RETURN(fd);
    my_file_info[fd].name = my_strdup(key_memory_my_file_info, FileName, MyFlags);
    statistic_increment(my_file_total_opened,&THR_LOCK_open);
    my_file_info[fd].type = type_of_file;
    DBUG_PRINT("exit",("fd: %d",fd));
    DBUG_RETURN(fd);
  }
  my_errno= errno;

  DBUG_PRINT("error",("Got error %d on open", my_errno));
  if (MyFlags & (MY_FFNF | MY_FAE | MY_WME))
  {
    if (my_errno == EMFILE)
      error_message_number= EE_OUT_OF_FILERESOURCES;
    my_error(error_message_number,
             MYF(ME_BELL | (MyFlags & (ME_NOTE | ME_ERROR_LOG))),
             FileName, my_errno);
  }
  DBUG_RETURN(-1);
}
