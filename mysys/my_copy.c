/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates
   Copyright (c) 2026, MariaDB plc

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
#include "aligned.h"
#include <my_dir.h> /* for stat */
#include <m_string.h>
#ifdef __linux__
# include <sys/ioctl.h>
# include <linux/fs.h> /* FICLONE, FICLONERANGE (Linux 4.5) */
#endif

#if defined(HAVE_UTIME_H)
#include <utime.h>
#elif defined(HAVE_SYS_UTIME_H)
#include <sys/utime.h>
#elif !defined(HPUX10)
#include <time.h>
struct utimbuf {
  time_t actime;
  time_t modtime;
};
#endif

/* Size of the buffer used when copying a file */
#define COPY_BUFFER_SIZE (4 << 20)

/*
  Files of at least this size are copied without filling the file
  cache with data that we are not going to read again. For smaller
  files the cache is useful, as the copy is often read back at once.
*/
#define COPY_NO_BUFFERING_MIN_SIZE COPY_BUFFER_SIZE

#if defined __linux__ || defined __FreeBSD__
/**
   Copy or clone a file range via copy_file_range(2).
   @param in_fd   source file descriptor
   @param out_fd  destination file descriptor
   @param offset  first offset to be copied
   @param end     first offset not to be copied
   @return error code (non-positive)
   @retval 0 on success
   @retval -1 on unexpected end-of-file
*/
static int copy_with_copy_file_range(File from, File to,
                                     off_t offset, off_t end)
{
  DBUG_ASSERT(end >= offset);
  for (;;)
  {
    ssize_t ret;
    size_t c;
# ifdef __linux__
    DBUG_ASSERT(end - offset <= COPY_NO_BUFFERING_MIN_SIZE);
    c= (size_t) (end - offset);
# else
    /*
      FreeBSD copy_file_range(2) manual page:
      "there is no penalty for using very large len values, even SSIZE_MAX"
    */
    c= (size_t) MY_MIN(end - offset, (my_off_t) SSIZE_MAX);
# endif
    ret= copy_file_range(from, &offset, to, &offset, c, 0);
    if (ret < 0)
      return (int) ret;
    if (offset == end)
      return 0;
    if (!ret)
      return -1;
  }
}
#endif

#ifdef __linux__
/**
   Attempt to clone a range of a file to the same range of another file.

   The caller must ensure that there is no concurrent write to the range.

   @param from     file to read from, not affecting the current position
   @param to       file to write to, not affecting the current position
   @param start    first offset to clone
   @param length   length of the cloned area

   @return error code (non-positive)
   @retval 0 on success
   @retval 1 if the operation is not supported
*/

int my_clone_file_range(my_native_file from, my_native_file to,
                        uint64_t start, uint64_t length)
{
# ifdef FICLONERANGE
  struct file_clone_range fcr= {from, start, length, start};
  const int ret= ioctl(to, FICLONERANGE, &fcr);
  DBUG_ASSERT(ret <= 0);
  if (ret)
    switch (errno) {
    case EOPNOTSUPP: case EXDEV: case EINVAL: break;
    default: return ret;
    }
# endif
  return 1;
}
#endif

/**
   Copy a range of a file to the same range of another file.

   The data is written at the same offset as it was read from. This
   retains the holes of a sparse file and allows new ones to be created
   while copying the file in pieces, in any order.

   The caller must ensure that there is no concurrent write to the range.

   @param from     file to read from, not affecting the current position
   @param to       file to write to, not affecting the current position
   @param start    first offset to copy
   @param end      first offset not to copy

   @return error code (non-positive)
   @retval 0 on success
*/

int my_copy_file_range(my_native_file from, my_native_file to,
                       my_off_t start, my_off_t end)
{
#ifdef __FreeBSD__
  /* On FreeBSD, copy_file_range(2) always works */
  return copy_with_copy_file_range(from, to, start, end);
#else
  char *b;
  size_t size;
  uint64_t count= end - start;
  ssize_t ret;
  DBUG_ENTER("my_copy_file_range");
  DBUG_ASSERT(end >= start);

  if (!count)
    DBUG_RETURN(0);

# if SIZEOF_SIZE_T < 8
  size= 65536;
# else
  /* An integer multiple of 4096 bytes, up to COPY_BUFFER_SIZE */
  size= count >= COPY_BUFFER_SIZE
    ? COPY_BUFFER_SIZE : ((size_t) count + 4095) & ~4095;
# endif
  b= aligned_malloc(size, 4096);
  if (!b)
  {
    /*
      _aligned_malloc() and memalign() set errno, but posix_memalign()
      returns the error number. The alignment is valid, so it is ENOMEM.
    */
    my_errno= errno= ENOMEM;
    ret= -1;
  }
  else
  {
# ifdef __linux__
    my_bool first= TRUE;
# endif
    for (;;)
    {
      if (count < size)
        size= count;
      ret= IF_WIN(my_win_pread_low,pread)(from, b, size, start);
      if (ret > 0)
      {
        size_t size= (size_t) ret;
        uint64_t wo= start;
        const char *buf= b;
# ifdef POSIX_FADV_DONTNEED
        (void) posix_fadvise(from, start, size, POSIX_FADV_DONTNEED);
# endif
        start+= ret;
        count-= (uint64_t) ret;

        do
        {
          ssize_t wrote= IF_WIN(my_win_pwrite_low,pwrite)(to, buf, size, wo);
          DBUG_ASSERT(wrote <= (ssize_t) size);
          if (wrote < 0)
          {
            ret= (int) wrote;
            break;
          }
# ifdef __linux__
          sync_file_range(to, wo, wrote, SYNC_FILE_RANGE_WRITE);
          if (first)
            first= FALSE;
          else
          {
            sync_file_range(to, 0, wo,
                            SYNC_FILE_RANGE_WAIT_BEFORE |
                            SYNC_FILE_RANGE_WRITE |
                            SYNC_FILE_RANGE_WAIT_AFTER);
            posix_fadvise(to, 0, wo, POSIX_FADV_DONTNEED);
          }
# endif
          buf+= wrote;
          wo+= wrote;
          size-= (size_t) wrote;
        }
        while (size);
        DBUG_ASSERT(wo == start || ret < 0);
      }
      if (ret < 0)
      {
        /* pread() or pwrite() failed */
        IF_WIN(my_osmaperr(GetLastError()),);
        break;
      }
      if (!count)
      {
        ret= 0;
        break;
      }
      if (!ret)
      {
        ret= -1;
        break;
      }
    }
    aligned_free(b);
# ifdef __linux__
    sync_file_range(to, 0, start,
                    SYNC_FILE_RANGE_WAIT_BEFORE |
                    SYNC_FILE_RANGE_WRITE |
                    SYNC_FILE_RANGE_WAIT_AFTER);
    posix_fadvise(to, 0, start, POSIX_FADV_DONTNEED);
# endif
  }
  DBUG_RETURN((int) ret);
#endif
}


#ifdef _WIN32
/* Use my_copy() */
#elif defined __APPLE__
/* the macro my_copy_file() expands to fcopyfile() */
#else
/**
   Copy the rest of an open file to another open file.

   The copy starts at the current position of both files and ends at
   the end of the 'from' file. Neither file is closed.

   @param from     file to read from
   @param to       file to write to

   @return 0 on success
*/
int my_copy_file(my_native_file from, my_native_file to)
{
  int ret;
  uint64_t end;
# if defined __linux__ && defined FICLONE
  /*
    Attempt block-cloning. We cannot rely on copy_file_range(2) for
    this, because for copying large files a user-space loop with
    pread(2) and pwrite(2) makes much better use of the block cache.
  */
  ret= ioctl(to, FICLONE, from);
  DBUG_ASSERT(ret <= 0);
  if (ret)
    switch (errno) {
    case EOPNOTSUPP: case EXDEV: case EINVAL:
      /* Fall back to copying. */
      break;
    default:
      /* On a hard error, there is no point to continue. */
      return ret;
    }
# endif
  end= lseek(from, 0, SEEK_END);
# ifdef POSIX_FADV_NOREUSE
  (void) posix_fadvise(from, 0, 0, POSIX_FADV_NOREUSE);
  (void) posix_fadvise(to, 0, 0, POSIX_FADV_NOREUSE);
# endif
# ifdef __linux__
  if (end < COPY_NO_BUFFERING_MIN_SIZE)
  {
    /* For small files, try copy_file_range(2). */
    ret= copy_with_copy_file_range(from, to, 0, (my_off_t) end);
    DBUG_ASSERT(ret <= 0);
    if (ret)
      switch (errno) {
      case EOPNOTSUPP: case EXDEV:
        break;
      default:
        return ret;
      }
  }
# endif
# ifdef POSIX_FADV_SEQUENTIAL
  (void) posix_fadvise(from, 0, 0, POSIX_FADV_SEQUENTIAL);
# endif
  ret= my_copy_file_range(from, to, 0, end);
# ifdef POSIX_FADV_NORMAL
  (void) posix_fadvise(from, 0, 0, POSIX_FADV_NORMAL);
# endif
  return ret;
}
#endif


/*
  Copy a file

  NOTES
    Ordinary ownership and accesstimes are copied from 'from-file'
    If MyFlags & MY_HOLD_ORIGINAL_MODES is set and to-file exists then
    the modes of to-file isn't changed
    If MyFlags & MY_DONT_OVERWRITE_FILE is set, we will give an error
    if the file existed.

    On Windows CopyFileEx() lets the operating system do the copy,
    which is faster than moving the data through this process. It also
    copies the file attributes and the file times.

  WARNING
    Don't set MY_FNABP or MY_NABP bits on when calling this function !

  RETURN
    0	ok
    #	Error

*/

int my_copy(const char *from, const char *to, myf MyFlags)
{
  MY_STAT stat_buff, new_stat_buff, *mode_stat;
  File from_file= -1, to_file= -1;
  my_bool new_file_stat= 0;                 /* 1 if we could stat "to" */
  my_bool file_created= 0;
  DBUG_ENTER("my_copy");
  DBUG_PRINT("my",("from %s to %s MyFlags %lu", from, to, MyFlags));

  DBUG_ASSERT(!(MyFlags & (MY_FNABP | MY_NABP))); /* for my_read/my_write */

  if (MyFlags & MY_HOLD_ORIGINAL_MODES)     /* Copy stat if possible */
    new_file_stat= MY_TEST(my_stat((char*) to, &new_stat_buff, MYF(0)));

  if (!my_stat(from, &stat_buff, MYF(0)))
  {
    my_errno= errno;
    if (MyFlags & (MY_FAE | MY_WME))
      my_error(EE_FILENOTFOUND, MYF(ME_BELL), from, my_errno);
    DBUG_RETURN(-1);
  }
  /*
    stat_buff is the source; mode_stat decides the modes and the times
    of the copy, which are the ones of an existing target file when
    MY_HOLD_ORIGINAL_MODES is given.
  */
  mode_stat= ((MyFlags & MY_HOLD_ORIGINAL_MODES) && new_file_stat ?
              &new_stat_buff : &stat_buff);

#ifdef _WIN32
  {
    DWORD copy_flags= ((MyFlags & MY_DONT_OVERWRITE_FILE) ?
                       COPY_FILE_FAIL_IF_EXISTS : 0);
    int retries= FILE_SHARING_VIOLATION_RETRIES;

    if ((my_off_t) stat_buff.st_size >= COPY_NO_BUFFERING_MIN_SIZE)
    {
      /*
        Microsoft recommends unbuffered copying of very big files, as
        it does not fill the file cache with the copied data.
      */
      copy_flags|= COPY_FILE_NO_BUFFERING;
    }
    DBUG_INJECT_FILE_SHARING_VIOLATION(from);

    while (!CopyFileEx(from, to, NULL, NULL, NULL, copy_flags))
    {
      DWORD last_error= GetLastError();
      DBUG_CLEAR_FILE_SHARING_VIOLATION();
      /*
        Another process may have the file open or locked for a short
        time, which is common on Windows because of virus scanners,
        indexers and backup programs. Retry a limited number of times.
      */
      if ((last_error == ERROR_SHARING_VIOLATION ||
           last_error == ERROR_LOCK_VIOLATION) && --retries > 0)
      {
        Sleep(FILE_SHARING_VIOLATION_DELAY_MS);
        continue;
      }
      my_osmaperr(last_error);
      my_errno= errno;
      if (MyFlags & (MY_FAE | MY_WME))
        my_error(EE_CANTCREATEFILE, MYF(ME_BELL), to, my_errno);
      if (my_errno != EEXIST)
        file_created= 1;                    /* Remove the partial copy */
      goto err;
    }
    DBUG_CLEAR_FILE_SHARING_VIOLATION();
    file_created= 1;
    /* CopyFileEx() gives us no file to sync; open the copy if needed */
    if ((MyFlags & MY_SYNC) &&
        ((to_file= my_open(to, O_WRONLY | O_BINARY, MyFlags)) < 0 ||
         my_sync(to_file, MyFlags)))
      goto err;
  }
#else
  {
    int create_flag= (MyFlags & MY_DONT_OVERWRITE_FILE) ? O_EXCL : O_TRUNC;

    if ((from_file= my_open(from, O_RDONLY | O_SHARE, MyFlags)) < 0)
      goto err;
    if ((to_file= my_create(to, (int) mode_stat->st_mode,
                            O_WRONLY | create_flag | O_BINARY | O_SHARE,
                            MyFlags)) < 0)
      goto err;
    file_created= 1;
    if (my_copy_file(from_file, to_file))
      goto err;
    if ((MyFlags & MY_SYNC) && my_sync(to_file, MyFlags))
      goto err;
  }
#endif

  if ((from_file >= 0 && my_close(from_file, MyFlags)) ||
      (to_file   >= 0 && my_close(to_file, MyFlags)))
    DBUG_RETURN(-1);                        /* Error on close */
  from_file= to_file= -1;                   /* Files are closed */

  /* Copy modes if possible */

  if (MyFlags & MY_HOLD_ORIGINAL_MODES && !new_file_stat)
    DBUG_RETURN(0);                         /* File copied but not stat */

  if (chmod(to, mode_stat->st_mode & 07777))
  {
    my_errno= errno;
    if (MyFlags & MY_WME)
      my_error(EE_CHANGE_PERMISSIONS, MYF(ME_BELL), to, errno);
    if (MyFlags & MY_FAE)
      goto err;
  }
#ifndef _WIN32
  /* Copy ownership */
  if (chown(to, mode_stat->st_uid, mode_stat->st_gid))
  {
    my_errno= errno;
    if (MyFlags & MY_WME)
      my_error(EE_CANT_COPY_OWNERSHIP, MYF(ME_BELL), to, errno);
    if (MyFlags & MY_FAE)
      goto err;
  }
#endif

  if (MyFlags & MY_COPYTIME)
  {
    struct utimbuf timep;
    timep.actime  = mode_stat->st_atime;
    timep.modtime = mode_stat->st_mtime;
    (void) utime((char*) to, &timep); /* last accessed and modified times */
  }

  DBUG_RETURN(0);

err:
  if (from_file >= 0) (void) my_close(from_file, MyFlags);
  if (to_file >= 0)   (void) my_close(to_file, MyFlags);

  /* attempt to delete the to-file we've partially written */
  if (file_created)
    (void) my_delete(to, MyFlags);

  DBUG_RETURN(-1);
} /* my_copy */
