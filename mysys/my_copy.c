/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates

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
#include <my_dir.h> /* for stat */
#include <m_string.h>
#ifdef HAVE_FCOPYFILE
#include <copyfile.h>
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
#define COPY_BUFFER_SIZE (1024*1024)

/*
  Files of at least this size are copied without filling the file
  cache with data that we are not going to read again. For smaller
  files the cache is useful, as the copy is often read back at once.
*/
#define COPY_NO_BUFFERING_MIN_SIZE (64*1024*1024)

#ifdef _WIN32
/* Not defined in old Windows SDK:s */
#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif
#endif

#ifdef HAVE_COPY_FILE_RANGE

/*
  Copy the rest of a file with copy_file_range(), which lets the file
  system copy the data without moving it through user space. Given the
  length of the whole file, a file system like Btrfs or XFS can share
  the blocks with the original file instead of copying them.

  copy_file_range() is allowed to copy less than we ask for, in which
  case we call it again for the rest of the file.

  If the copy fails in the middle, the file positions are still
  updated, which allows the caller to copy the rest of the file in the
  normal way. The error is then reported by the normal copy loop.

  @return 0 if the whole file was copied
  @return 1 if the caller has to copy the rest of the file
*/

static int copy_with_copy_file_range(File from, File to, myf MyFlags)
{
  MY_STAT stat_buff;
  my_off_t position, length;

  if (my_fstat(from, &stat_buff, MyFlags) ||
      !MY_S_ISREG(stat_buff.st_mode))
    return 1;                                   /* Not a normal file */
  position= my_tell(from, MyFlags);
  if (position == (my_off_t) -1 ||
      position >= (my_off_t) stat_buff.st_size)
    return 1;
  length= (my_off_t) stat_buff.st_size - position;

  while (length)
  {
    size_t chunk= (size_t) MY_MIN(length, (my_off_t) SIZE_T_MAX / 2);
    ssize_t copied= copy_file_range(from, NULL, to, NULL, chunk, 0);
    if (copied <= 0)
      return 1;
    length-= (my_off_t) copied;
  }
  return 0;
}


/*
  Copy a range of a file with copy_file_range().

  The offsets are given to copy_file_range(), which means that the
  data is written at the same offset as it was read from.

  If the copy fails in the middle, the caller can safely copy the
  whole range again, as nothing is written outside the range.

  @return 0 if the whole range was copied
  @return 1 if the caller has to copy the range in another way
*/

static int copy_range_with_copy_file_range(File from, File to,
                                           my_off_t start, my_off_t end)
{
  off_t from_pos= (off_t) start, to_pos= (off_t) start;

  while ((my_off_t) from_pos < end)
  {
    size_t chunk= (size_t) MY_MIN(end - (my_off_t) from_pos,
                                  (my_off_t) SIZE_T_MAX / 2);
    if (copy_file_range(from, &from_pos, to, &to_pos, chunk, 0) <= 0)
      return 1;
  }
  return 0;
}

#endif /* HAVE_COPY_FILE_RANGE */


#ifdef HAVE_MMAP

/*
  Copy a range of a file through a memory mapping of the source file,
  which avoids copying the data into a buffer of our own.

  @return 0 if the whole range was copied
  @return 1 if the caller has to copy the range in another way
*/

static int copy_range_with_mmap(File from, File to, my_off_t start,
                                my_off_t end, myf MyFlags)
{
  void *map;
  int error;

  if (sizeof(size_t) <= 4 || end > (my_off_t) (SIZE_T_MAX / 2))
    return 1;                                   /* Too big to map */
  map= my_mmap(0, (size_t) end, PROT_READ, MAP_SHARED, from, 0);
  if (map == MAP_FAILED)
    return 1;

  error= MY_TEST(my_pwrite(to, (const uchar*) map + start,
                           (size_t) (end - start), start,
                           MYF(MyFlags | MY_NABP)));
  my_munmap(map, (size_t) end);
  return error;
}

#endif /* HAVE_MMAP */


/*
  Copy a range of a file to the same range of another file.

  Unlike my_copy_file(), the data is written at the same offset as it
  was read from. This keeps the holes of a sparse file and allows a
  file to be copied in pieces, in any order. The file positions of
  'from' and 'to' are not used and not changed.

  This is the function to use when a file is copied while it may be
  written to, in which case the caller has to ensure that the range
  that is copied is not modified while we copy it.

  @param from     file to read from
  @param to       file to write to
  @param start    first offset to copy
  @param end      last offset to copy (exclusive)
  @param MyFlags  flags for my_pread() and my_pwrite()

  @return 0 on success
*/

int my_copy_file_range(File from, File to, my_off_t start, my_off_t end,
                       myf MyFlags)
{
  uchar stack_buff[IO_SIZE], *buff;
  size_t buff_length;
  my_off_t position= start;
  int error= 0;
  DBUG_ENTER("my_copy_file_range");
  DBUG_ASSERT(end >= start);

  if (end == start)
    DBUG_RETURN(0);

#ifdef HAVE_COPY_FILE_RANGE
  if (!copy_range_with_copy_file_range(from, to, start, end))
    DBUG_RETURN(0);
#endif

/*
  copy_file_range does not work over different file systems.
  Fallback to memmap or a read/write loop with a large buffer.
*/
#ifdef HAVE_MMAP
  if (!copy_range_with_mmap(from, to, start, end, MyFlags))
    DBUG_RETURN(0);
#endif

  if ((buff= (uchar*) my_malloc(PSI_INSTRUMENT_ME, COPY_BUFFER_SIZE,
                                MYF(0))))
    buff_length= COPY_BUFFER_SIZE;
  else
  {
    buff= stack_buff;                           /* Out of memory */
    buff_length= sizeof(stack_buff);
  }

  while (position < end)
  {
    size_t length= (size_t) MY_MIN(end - position, (my_off_t) buff_length);
    if (my_pread(from, buff, length, position, MYF(MyFlags | MY_NABP)) ||
        my_pwrite(to, buff, length, position, MYF(MyFlags | MY_NABP)))
    {
      error= -1;
      break;
    }
    position+= length;
  }

  if (buff != stack_buff)
    my_free(buff);
  DBUG_RETURN(error);
}


/*
  Copy the rest of an open file to another open file.

  The copy starts at the current position of both files and ends at
  the end of the 'from' file. Neither file is closed.

  @param from     file to read from
  @param to       file to write to
  @param MyFlags  flags for my_read() and my_write()

  @return 0 on success
*/

int my_copy_file(File from, File to, myf MyFlags)
{
  int error= 0, copied= 0;
#if defined(POSIX_FADV_SEQUENTIAL) || defined(POSIX_FADV_DONTNEED)
  MY_STAT stat_buff;
  /*
    Only a big file is worth telling the operating system about; for a
    small file the copy is cheap and the file cache may be useful.
  */
  my_bool big_file= (!my_fstat(from, &stat_buff, MYF(0)) &&
                     (my_off_t) stat_buff.st_size >=
                     COPY_NO_BUFFERING_MIN_SIZE);
#endif
  DBUG_ENTER("my_copy_file");

  DBUG_ASSERT(!(MyFlags & (MY_FNABP | MY_NABP)));

#ifdef POSIX_FADV_SEQUENTIAL
  /* We are going to read the whole file, from start to end */
  if (big_file)
    (void) posix_fadvise(from, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#ifdef HAVE_FCOPYFILE
  /*
    On macOS fcopyfile() lets the operating system do the copy and on
    some file systems it can clone the file instead of copying it.
  */
  copied= !fcopyfile(from, to, NULL, COPYFILE_ALL | COPYFILE_CLONE);
#endif
#ifdef HAVE_COPY_FILE_RANGE
  if (!copied)
    copied= !copy_with_copy_file_range(from, to, MyFlags);
#endif

  if (!copied)
  {
    uchar stack_buff[IO_SIZE], *buff;
    size_t buff_length, length;

    if ((buff= (uchar*) my_malloc(PSI_INSTRUMENT_ME, COPY_BUFFER_SIZE,
                                  MYF(0))))
      buff_length= COPY_BUFFER_SIZE;
    else
    {
      buff= stack_buff;                         /* Out of memory */
      buff_length= sizeof(stack_buff);
    }

    while ((length= my_read(from, buff, buff_length, MyFlags)))
    {
      if (length == MY_FILE_ERROR ||
          my_write(to, buff, length, MYF(MyFlags | MY_NABP)))
      {
        error= -1;
        break;
      }
    }

    if (buff != stack_buff)
      my_free(buff);
  }

#ifdef POSIX_FADV_DONTNEED
  /* We will not read the file again; do not keep it in the page cache */
  if (big_file)
    (void) posix_fadvise(from, 0, 0, POSIX_FADV_DONTNEED);
#endif
  DBUG_RETURN(error);
}


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
    if (my_copy_file(from_file, to_file, MyFlags))
      goto err;
    if ((MyFlags & MY_SYNC) && my_sync(to_file, MyFlags))
      goto err;
  }
#endif

  if ((from_file >= 0 && my_close(from_file, MyFlags)) |
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
