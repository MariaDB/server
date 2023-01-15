/*
   Copyright (c) 2001, 2011, Oracle and/or its affiliates
   Copyright (c) 2010, 2017, MariaDB

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
#ifdef HAVE_REALPATH
#include <sys/param.h>
#include <sys/stat.h>
#endif

static int always_valid(const char *filename __attribute__((unused)))
{
  return 0;
}

int (*mysys_test_invalid_symlink)(const char *filename)= always_valid;


/*
  Reads the content of a symbolic link
  If the file is not a symbolic link, return the original file name in to.

  RETURN
    0  If filename was a symlink,    (to will be set to value of symlink)
    1  If filename was a normal file (to will be set to filename)
   -1  on error.
*/

int my_readlink(char *to, const char *filename, myf MyFlags)
{
#ifndef HAVE_READLINK
  strmov(to,filename);
  return 1;
#else
  int result=0;
  int length;
  DBUG_ENTER("my_readlink");

  if ((length=readlink(filename, to, FN_REFLEN-1)) < 0)
  {
    /* Don't give an error if this wasn't a symlink */
    if ((my_errno=errno) == EINVAL)
    {
      result= 1;
      strmov(to,filename);
    }
    else
    {
      if (MyFlags & MY_WME)
	my_error(EE_CANT_READLINK, MYF(0), filename, errno);
      result= -1;
    }
  }
  else
    to[length]=0;
  DBUG_PRINT("exit" ,("result: %d", result));
  DBUG_RETURN(result);
#endif /* HAVE_READLINK */
}


/* Create a symbolic link */

int my_symlink(const char *content, const char *linkname, myf MyFlags)
{
#ifndef HAVE_READLINK
  return 0;
#else
  int result;
  DBUG_ENTER("my_symlink");
  DBUG_PRINT("enter",("content: %s  linkname: %s", content, linkname));

  result= 0;
  if (symlink(content, linkname))
  {
    result= -1;
    my_errno=errno;
    if (MyFlags & MY_WME)
      my_error(EE_CANT_SYMLINK, MYF(0), linkname, content, errno);
  }
  else if ((MyFlags & MY_SYNC_DIR) && my_sync_dir_by_file(linkname, MyFlags))
    result= -1;
  DBUG_RETURN(result);
#endif /* HAVE_READLINK */
}

#if defined(SCO)
#define BUFF_LEN 4097
#elif defined(MAXPATHLEN)
#define BUFF_LEN MAXPATHLEN
#else
#define BUFF_LEN FN_LEN
#endif


int my_is_symlink(const char *filename __attribute__((unused)))
{
#if defined (HAVE_LSTAT) && defined (S_ISLNK)
  struct stat stat_buff;
  return !lstat(filename, &stat_buff) && S_ISLNK(stat_buff.st_mode);
#elif defined (_WIN32)
  DWORD dwAttr = GetFileAttributes(filename);
  return (dwAttr != INVALID_FILE_ATTRIBUTES) &&
    (dwAttr & FILE_ATTRIBUTE_REPARSE_POINT);
#else  /* No symlinks */
  return 0;
#endif
}

/*
  Resolve all symbolic links in path
  'to' may be equal to 'filename'

  to is guaranteed to never set to a string longer than FN_REFLEN
  (including the end \0)

  On error returns -1, unless error is file not found, in which case it
  is 1.

  Sets my_errno to specific error number.
*/

int my_realpath(char *to, const char *filename, myf MyFlags)
{
#if defined(HAVE_REALPATH) && !defined(HAVE_BROKEN_REALPATH)
  int result=0;
  char buff[BUFF_LEN];
  char *ptr;
  DBUG_ENTER("my_realpath");

  DBUG_PRINT("info",("executing realpath"));
  if ((ptr=realpath(filename,buff)))
    strmake(to, ptr, FN_REFLEN-1);
  else
  {
    /*
      Realpath didn't work;  Use my_load_path() which is a poor substitute
      original name but will at least be able to resolve paths that starts
      with '.'.
    */
    DBUG_PRINT("error",("realpath failed with errno: %d", errno));
    my_errno=errno;
    if (MyFlags & MY_WME)
      my_error(EE_REALPATH, MYF(0), filename, my_errno);
    my_load_path(to, filename, NullS);
    if (my_errno == ENOENT)
      result= 1;
    else
      result= -1;
  }
  DBUG_RETURN(result);
#elif defined(_WIN32)
  int ret= GetFullPathName(filename,FN_REFLEN, to, NULL);
  if (ret == 0 || ret > FN_REFLEN)
  {
    my_errno= (ret > FN_REFLEN) ? ENAMETOOLONG : GetLastError();
    if (MyFlags & MY_WME)
      my_error(EE_REALPATH, MYF(0), filename, my_errno);
    /* 
      GetFullPathName didn't work : use my_load_path() which is a poor 
      substitute original name but will at least be able to resolve 
      paths that starts with '.'.
    */  
    my_load_path(to, filename, NullS);
    return -1;
  }
#else
  my_load_path(to, filename, NullS);
#endif
  return 0;
}

#ifdef HAVE_OPEN_PARENT_DIR_NOSYMLINKS
/** opens the parent dir. walks the path, and does not resolve symlinks

   returns the pointer to the file name (basename) within the pathname
   or NULL in case of an error

   stores the parent dir (dirname) file descriptor in pdfd.
   It can be -1 even if there was no error!

   This is used for symlinked tables for DATA/INDEX DIRECTORY.
   The paths there have been realpath()-ed. So, we can assume here that

    * `pathname` is an absolute path
    * no '.', '..', and '//' in the path
    * file exists
*/

const char *my_open_parent_dir_nosymlinks(const char *pathname, int *pdfd)
{
  char buf[FN_REFLEN + 1];
  char *s= buf, *e= buf+1, *end= strnmov(buf, pathname, sizeof(buf));
  int fd, dfd= -1;

  if (*end)
  {
    errno= ENAMETOOLONG;
    return NULL;
  }

  if (*s != '/') /* not an absolute path */
  {
    errno= ENOENT;
    return NULL;
  }

  for (;;)
  {
    if (*e == '/') /* '//' in the path */
    {
      errno= ENOENT;
      goto err;
    }
    while (*e && *e != '/')
      e++;
    *e= 0;

    if (!memcmp(s, ".", 2) || !memcmp(s, "..", 3))
    {
      errno= ENOENT;
      goto err;
    }

    if (++e >= end)
    {
      *pdfd= dfd;
      return pathname + (s - buf);
    }

    fd = openat(dfd, s, O_NOFOLLOW | O_PATH | O_CLOEXEC);
    if (fd < 0)
      goto err;

    if (dfd >= 0)
      close(dfd);

    dfd= fd;
    s= e;
  }
err:
  if (dfd >= 0)
    close(dfd);
  return NULL;
}
#endif
