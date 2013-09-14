/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates
   Copyright (c) 1995, 2013, Monty Program Ab

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

#include "mysys_priv.h"
#include "mysys_err.h"

#ifndef SHARED_LIBRARY

const char *globerrs[GLOBERRS]=
{
  "Can't create/write to file '%s' (Errcode: %M)",
  "Error reading file '%s' (Errcode: %M)",
  "Error writing file '%s' (Errcode: %M)",
  "Error on close of '%s' (Errcode: %M)",
  "Out of memory (Needed %u bytes)",
  "Error on delete of '%s' (Errcode: %M)",
  "Error on rename of '%s' to '%s' (Errcode: %M)",
  "",
  "Unexpected end-of-file found when reading file '%s' (Errcode: %M)",
  "Can't lock file (Errcode: %M)",
  "Can't unlock file (Errcode: %M)",
  "Can't read dir of '%s' (Errcode: %M)",
  "Can't get stat of '%s' (Errcode: %M)",
  "Can't change size of file (Errcode: %M)",
  "Can't open stream from handle (Errcode: %M)",
  "Can't get working directory (Errcode: %M)",
  "Can't change dir to '%s' (Errcode: %M)",
  "Warning: '%s' had %d links",
  "Warning: %d files and %d streams is left open\n",
  "Disk is full writing '%s' (Errcode: %M). Waiting for someone to free space... (Expect up to %d secs delay for server to continue after freeing disk space)",
  "Can't create directory '%s' (Errcode: %M)",
  "Character set '%s' is not a compiled character set and is not specified in the '%s' file",
  "Out of resources when opening file '%s' (Errcode: %M)",
  "Can't read value for symlink '%s' (Errcode: %M)",
  "Can't create symlink '%s' pointing at '%s' (Errcode: %M)",
  "Error on realpath() on '%s' (Errcode: %M)",
  "Can't sync file '%s' to disk (Errcode: %M)",
  "Collation '%s' is not a compiled collation and is not specified in the '%s' file",
  "File '%s' not found (Errcode: %M)",
  "File '%s' (fileno: %d) was not closed",
  "Can't change ownership of the file '%s' (Errcode: %M)",
  "Can't change permissions of the file '%s' (Errcode: %M)",
  "Can't seek in file '%s' (Errcode: %M)",
  "Can't change mode for file '%s' to 0x%lx (Errcode: %M)",
  "Warning: Can't copy ownership for file '%s' (Errcode: %M)"
};

void init_glob_errs(void)
{
  /* This is now done statically. */
}

#else

void init_glob_errs()
{
  EE(EE_CANTCREATEFILE) = "Can't create/write to file '%s' (Errcode: %M)";
  EE(EE_READ)		= "Error reading file '%s' (Errcode: %M)";
  EE(EE_WRITE)		= "Error writing file '%s' (Errcode: %M)";
  EE(EE_BADCLOSE)	= "Error on close of '%'s (Errcode: %M)";
  EE(EE_OUTOFMEMORY)	= "Out of memory (Needed %u bytes)";
  EE(EE_DELETE)		= "Error on delete of '%s' (Errcode: %M)";
  EE(EE_LINK)		= "Error on rename of '%s' to '%s' (Errcode: %M)";
  EE(EE_EOFERR)		= "Unexpected eof found when reading file '%s' (Errcode: %M)";
  EE(EE_CANTLOCK)	= "Can't lock file (Errcode: %M)";
  EE(EE_CANTUNLOCK)	= "Can't unlock file (Errcode: %M)";
  EE(EE_DIR)		= "Can't read dir of '%s' (Errcode: %M)";
  EE(EE_STAT)		= "Can't get stat of '%s' (Errcode: %M)";
  EE(EE_CANT_CHSIZE)	= "Can't change size of file (Errcode: %M)";
  EE(EE_CANT_OPEN_STREAM)= "Can't open stream from handle (Errcode: %M)";
  EE(EE_GETWD)		= "Can't get working directory (Errcode: %M)";
  EE(EE_SETWD)		= "Can't change dir to '%s' (Errcode: %M)";
  EE(EE_LINK_WARNING)	= "Warning: '%s' had %d links";
  EE(EE_OPEN_WARNING)	= "Warning: %d files and %d streams is left open\n";
  EE(EE_DISK_FULL)	= "Disk is full writing '%s' (Errcode: %M). Waiting for someone to free space... (Expect up to %d secs delay for server to continue after freeing disk space)",
  EE(EE_CANT_MKDIR)	="Can't create directory '%s' (Errcode: %M)";
  EE(EE_UNKNOWN_CHARSET)= "Character set '%s' is not a compiled character set and is not specified in the %s file";
  EE(EE_OUT_OF_FILERESOURCES)="Out of resources when opening file '%s' (Errcode: %M)";
  EE(EE_CANT_READLINK)=	"Can't read value for symlink '%s' (Errcode: %M)";
  EE(EE_CANT_SYMLINK)=	"Can't create symlink '%s' pointing at '%s' (Errcode: %M)";
  EE(EE_REALPATH)=	"Error on realpath() on '%s' (Errcode: %M)";
  EE(EE_SYNC)=		"Can't sync file '%s' to disk (Errcode: %M)";
  EE(EE_UNKNOWN_COLLATION)= "Collation '%s' is not a compiled collation and is not specified in the %s file";
  EE(EE_FILENOTFOUND)	= "File '%s' not found (Errcode: %M)";
  EE(EE_FILE_NOT_CLOSED) = "File '%s' (fileno: %d) was not closed";
  EE(EE_CHANGE_OWNERSHIP)   = "Can't change ownership of the file '%s' (Errcode: %M)";
  EE(EE_CHANGE_PERMISSIONS) = "Can't change permissions of the file '%s' (Errcode: %M)";
  EE(EE_CANT_SEEK)      = "Can't seek in file '%s' (Errcode: %M)";
  EE(EE_CANT_CHMOD)    = "Can't change mode for file '%s' to 0x%lx (Errcode: %M)";
  EE(EE_CANT_COPY_OWNERSHIP)= "Warning: Can't copy ownership for file '%s' (Errcode: %M)";
}
#endif

void wait_for_free_space(const char *filename, int errors)
{
  if (errors == 0)
    my_error(EE_DISK_FULL,MYF(ME_BELL | ME_NOREFRESH | ME_JUST_WARNING),
             filename,my_errno,MY_WAIT_FOR_USER_TO_FIX_PANIC);
  if (!(errors % MY_WAIT_GIVE_USER_A_MESSAGE))
    my_printf_error(EE_DISK_FULL,
                    "Retry in %d secs. Message reprinted in %d secs",
                    MYF(ME_BELL | ME_NOREFRESH | ME_JUST_WARNING),
                    MY_WAIT_FOR_USER_TO_FIX_PANIC,
                    MY_WAIT_GIVE_USER_A_MESSAGE * MY_WAIT_FOR_USER_TO_FIX_PANIC );
  (void) sleep(MY_WAIT_FOR_USER_TO_FIX_PANIC);
}

const char **get_global_errmsgs()
{
  return globerrs;
}
