/*
   Copyright (c) 2003, 2011, Oracle and/or its affiliates

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
#include <errno.h>


ulong my_sync_count;                           /* Count number of sync calls */

static void (*before_sync_wait)(void)= 0;
static void (*after_sync_wait)(void)= 0;

void thr_set_sync_wait_callback(void (*before_wait)(void),
                                void (*after_wait)(void))
{
  before_sync_wait= before_wait;
  after_sync_wait= after_wait;
}

/*
  Sync data in file to disk

  SYNOPSIS
    my_sync()
    fd			File descriptor to sync
    my_flags		Flags (now only MY_WME is supported)

  NOTE
    If file system supports its, only file data is synced, not inode data.

    MY_IGNORE_BADFD is useful when fd is "volatile" - not protected by a
    mutex. In this case by the time of fsync(), fd may be already closed by
    another thread, or even reassigned to a different file. With this flag -
    MY_IGNORE_BADFD - such a situation will not be considered an error.
    (which is correct behaviour, if we know that the other thread synced the
    file before closing)

  RETURN
    0 ok
    -1 error
*/

int my_sync(File fd, myf my_flags)
{
  int res;
  DBUG_ENTER("my_sync");
  DBUG_PRINT("my",("fd: %d  my_flags: %lu", fd, my_flags));

  if (my_disable_sync)
    DBUG_RETURN(0);

  statistic_increment(my_sync_count,&THR_LOCK_open);

  if (before_sync_wait)
    (*before_sync_wait)();

  do
  {
#if defined(F_FULLFSYNC)
    /*
      In Mac OS X >= 10.3 this call is safer than fsync() (it forces the
      disk's cache and guarantees ordered writes).
    */
    if (!(res= fcntl(fd, F_FULLFSYNC, 0)))
      break; /* ok */
    /* Some file systems don't support F_FULLFSYNC and fail above: */
    DBUG_PRINT("info",("fcntl(F_FULLFSYNC) failed, falling back"));
#endif
#if defined(HAVE_FDATASYNC) && HAVE_DECL_FDATASYNC
    res= fdatasync(fd);
#elif defined(HAVE_FSYNC)
    res= fsync(fd);
    if (res == -1 && errno == ENOLCK)
      res= 0;                                   /* Result Bug in Old FreeBSD */
#elif defined(_WIN32)
    res= my_win_fsync(fd);
#else
#error Cannot find a way to sync a file, durability in danger
    res= 0;					/* No sync (strange OS) */
#endif
  } while (res == -1 && errno == EINTR);

  if (res)
  {
    int er= errno;
    if (!(my_errno= er))
      my_errno= -1;                             /* Unknown error */
    if (after_sync_wait)
      (*after_sync_wait)();
    if ((my_flags & MY_IGNORE_BADFD) &&
        (er == EBADF || er == EINVAL || er == EROFS))
    {
      DBUG_PRINT("info", ("ignoring errno %d", er));
      res= 0;
    }
    else if (my_flags & MY_WME)
      my_error(EE_SYNC, MYF(ME_BELL), my_filename(fd), my_errno);
  }
  else
  {
    if (after_sync_wait)
      (*after_sync_wait)();
  }
  DBUG_RETURN(res);
} /* my_sync */


/*
  Force directory information to disk.

  SYNOPSIS
    my_sync_dir()
    dir_name             the name of the directory
    my_flags             flags (MY_WME etc)

  RETURN
    0 if ok, !=0 if error
*/

int my_sync_dir(const char *dir_name __attribute__((unused)),
                myf my_flags __attribute__((unused)))
{
#ifdef NEED_EXPLICIT_SYNC_DIR
  static const char cur_dir_name[]= {FN_CURLIB, 0};
  File dir_fd;
  int res= 0;
  const char *correct_dir_name;
  DBUG_ENTER("my_sync_dir");
  DBUG_PRINT("my",("Dir: '%s'  my_flags: %lu", dir_name, my_flags));
  /* Sometimes the path does not contain an explicit directory */
  correct_dir_name= (dir_name[0] == 0) ? cur_dir_name : dir_name;
  /*
    Syncing a dir may give EINVAL on tmpfs on Linux, which is ok.
    EIO on the other hand is very important. Hence MY_IGNORE_BADFD.
  */
  if ((dir_fd= my_open(correct_dir_name, O_RDONLY, MYF(my_flags))) >= 0)
  {
    if (my_sync(dir_fd, MYF(my_flags | MY_IGNORE_BADFD)))
      res= 2;
    if (my_close(dir_fd, MYF(my_flags)))
      res= 3;
  }
  else
    res= 1;
  DBUG_RETURN(res);
#else
  return 0;
#endif
}

/*
  Force directory information to disk.

  SYNOPSIS
    my_sync_dir_by_file()
    file_name            the name of a file in the directory
    my_flags             flags (MY_WME etc)

  RETURN
    0 if ok, !=0 if error
*/

int my_sync_dir_by_file(const char *file_name __attribute__((unused)),
                        myf my_flags __attribute__((unused)))
{
#ifdef NEED_EXPLICIT_SYNC_DIR
  char dir_name[FN_REFLEN];
  size_t dir_name_length;
  dirname_part(dir_name, file_name, &dir_name_length);
  return my_sync_dir(dir_name, my_flags & ~MY_NOSYMLINKS);
#else
  return 0;
#endif
}
