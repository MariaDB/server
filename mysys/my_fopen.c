/* Copyright (c) 2000, 2012, Oracle and/or its affiliates
   Copyright (c) 1985, 2011, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mysys_priv.h"
#include "my_static.h"
#include <errno.h>
#include "mysys_err.h"
#include "my_atomic.h"

static void make_ftype(char * to,int flag);

/*
  Open a file as stream

  SYNOPSIS
    my_fopen()
    FileName	Path-name of file
    Flags	Read | write | append | trunc (like for open())
    MyFlags	Flags for handling errors

  RETURN
    0	Error
    #	File handler
*/

FILE *my_fopen(const char *filename, int flags, myf MyFlags)
{
  FILE *fd;
  char type[10];
  DBUG_ENTER("my_fopen");
  DBUG_PRINT("my",("Name: '%s'  flags: %d  MyFlags: %lu",
		   filename, flags, MyFlags));

  make_ftype(type,flags);

#ifdef _WIN32
  fd= my_win_fopen(filename, type);
#else
  fd= fopen(filename, type);
#endif
  if (fd != 0)
  {
    /*
      The test works if MY_NFILE < 128. The problem is that fileno() is char
      on some OS (SUNOS). Actually the filename save isn't that important
      so we can ignore if this doesn't work.
    */

    int filedesc= my_fileno(fd);
    if ((uint)filedesc >= my_file_limit)
    {
      statistic_increment(my_stream_opened,&THR_LOCK_open);
      DBUG_RETURN(fd);				/* safeguard */
    }
    my_file_info[filedesc].name= my_strdup(key_memory_my_file_info, filename, MyFlags);
    statistic_increment(my_stream_opened, &THR_LOCK_open);
    statistic_increment(my_file_total_opened, &THR_LOCK_open);
    my_file_info[filedesc].type= STREAM_BY_FOPEN;
    DBUG_PRINT("exit",("stream: %p", fd));
    DBUG_RETURN(fd);
  }
  else
    my_errno=errno;
  DBUG_PRINT("error",("Got error %d on open",my_errno));
  if (MyFlags & (MY_FFNF | MY_FAE | MY_WME))
    my_error((flags & O_RDONLY) ? EE_FILENOTFOUND : EE_CANTCREATEFILE,
	     MYF(ME_BELL), filename, my_errno);
  DBUG_RETURN((FILE*) 0);
} /* my_fopen */


#if defined(_WIN32)

static FILE *my_win_freopen(const char *path, const char *mode, FILE *stream)
{
  int handle_fd, fd= _fileno(stream);
  HANDLE osfh;

  DBUG_ASSERT(path && stream);
  DBUG_ASSERT(strchr(mode, 'a')); /* We use FILE_APPEND_DATA below */

  /* Services don't have stdout/stderr on Windows, so _fileno returns -1. */
  if (fd < 0)
  {
    if (!freopen(path, mode, stream))
      return NULL;

    fd= _fileno(stream);
  }

  if ((osfh= CreateFile(path, GENERIC_READ | FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        NULL)) == INVALID_HANDLE_VALUE)
    return NULL;

  if ((handle_fd= _open_osfhandle((intptr_t)osfh, _O_TEXT)) == -1)
  {
    CloseHandle(osfh);
    return NULL;
  }

  if (_dup2(handle_fd, fd) < 0)
  {
    CloseHandle(osfh);
    return NULL;
  }

  _close(handle_fd);

  return stream;
}

#endif


/**
  Change the file associated with a file stream.

  @param path   Path to file.
  @param mode   Mode of the stream.
  @param stream File stream.

  @note
    This function is used to redirect stdout and stderr to a file and
    subsequently to close and reopen that file for log rotation.

  @retval A FILE pointer on success. Otherwise, NULL.
*/

FILE *my_freopen(const char *path, const char *mode, FILE *stream)
{
  FILE *result;

#if defined(_WIN32)
  result= my_win_freopen(path, mode, stream);
#else
  result= freopen(path, mode, stream);
#endif

  return result;
}


/* Close a stream */
int my_fclose(FILE *fd, myf MyFlags)
{
  int err,file;
  char *name= NULL;
  DBUG_ENTER("my_fclose");
  DBUG_PRINT("my",("stream: %p  MyFlags: %lu", fd, MyFlags));

  file= my_fileno(fd);
  if ((uint) file < my_file_limit && my_file_info[file].type != UNOPEN)
  {
    name= my_file_info[file].name;
    my_file_info[file].name= NULL;
    my_file_info[file].type= UNOPEN;
  }
#ifndef _WIN32
  err= fclose(fd);
#else
  err= my_win_fclose(fd);
#endif
  if(err < 0)
  {
    my_errno=errno;
    if (MyFlags & (MY_FAE | MY_WME))
      my_error(EE_BADCLOSE, MYF(ME_BELL), name, errno);
  }
  else
    statistic_decrement(my_stream_opened, &THR_LOCK_open);

  if (name)
  {
    my_free(name);
  }
  DBUG_RETURN(err);
} /* my_fclose */


	/* Make a stream out of a file handle */
	/* Name may be 0 */

FILE *my_fdopen(File Filedes, const char *name, int Flags, myf MyFlags)
{
  FILE *fd;
  char type[5];
  DBUG_ENTER("my_fdopen");
  DBUG_PRINT("my",("fd: %d  Flags: %d  MyFlags: %lu",
		   Filedes, Flags, MyFlags));

  make_ftype(type,Flags);
#ifdef _WIN32
  fd= my_win_fdopen(Filedes, type);
#else
  fd= fdopen(Filedes, type);
#endif
  if (!fd)
  {
    my_errno=errno;
    if (MyFlags & (MY_FAE | MY_WME))
      my_error(EE_CANT_OPEN_STREAM, MYF(ME_BELL), errno);
  }
  else
  {
    statistic_increment(my_stream_opened, &THR_LOCK_open);
    if ((uint) Filedes < (uint) my_file_limit)
    {
      if (my_file_info[Filedes].type != UNOPEN)
      {
        /* File is opened with my_open ! */
        my_atomic_add32_explicit(&my_file_opened, -1, MY_MEMORY_ORDER_RELAXED);
      }
      else
      {
        my_file_info[Filedes].name= my_strdup(key_memory_my_file_info,
                                              name, MyFlags);
      }
      my_file_info[Filedes].type= STREAM_BY_FDOPEN;
    }
  }

  DBUG_PRINT("exit",("stream: %p", fd));
  DBUG_RETURN(fd);
} /* my_fdopen */


/*   
  Make a fopen() typestring from a open() type bitmap

  SYNOPSIS
    make_ftype()
    to		String for fopen() is stored here
    flag	Flag used by open()

  IMPLEMENTATION
    This routine attempts to find the best possible match 
    between  a numeric option and a string option that could be 
    fed to fopen.  There is not a 1 to 1 mapping between the two.  
  
  NOTE
    On Unix, O_RDONLY is usually 0

  MAPPING
    r  == O_RDONLY   
    w  == O_WRONLY|O_TRUNC|O_CREAT  
    a  == O_WRONLY|O_APPEND|O_CREAT  
    r+ == O_RDWR  
    w+ == O_RDWR|O_TRUNC|O_CREAT  
    a+ == O_RDWR|O_APPEND|O_CREAT
    b  == FILE_BINARY
    e  == O_CLOEXEC
*/

static void make_ftype(register char * to, register int flag)
{
  /* check some possible invalid combinations */  
  DBUG_ASSERT((flag & (O_TRUNC | O_APPEND)) != (O_TRUNC | O_APPEND));
  DBUG_ASSERT((flag & (O_WRONLY | O_RDWR)) != (O_WRONLY | O_RDWR));

  if ((flag & (O_RDONLY|O_WRONLY)) == O_WRONLY)    
    *to++= (flag & O_APPEND) ? 'a' : 'w';  
  else if (flag & O_RDWR)          
  {
    /* Add '+' after theese */    
    if (flag & (O_TRUNC | O_CREAT))      
      *to++= 'w';    
    else if (flag & O_APPEND)      
      *to++= 'a';    
    else      
      *to++= 'r';
    *to++= '+';  
  }  
  else    
    *to++= 'r';

  if (flag & FILE_BINARY)    
    *to++='b';

  if (O_CLOEXEC)
    *to++= 'e';

  *to='\0';
} /* make_ftype */
