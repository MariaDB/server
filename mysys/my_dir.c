/* Copyright (c) 2026, MariaDB plc

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

/*
  Traverse a directory one entry at a time.

  Unlike my_dir(), which reads the whole directory into memory, these
  functions keep only one entry at a time. This makes it possible for
  several threads to share one directory scan, where each thread
  handles the entry that it gets.

  A MY_NO_CACHE_DIR is not protected against concurrent use; a caller
  that shares one between threads has to provide its own mutex.

  Almost all code that differs between operating systems is collected
  in the helper functions below, so that each function in the
  interface has at most one #ifdef.
*/

#include "mysys_priv.h"
#include <m_string.h>
#include <m_ctype.h>
#include <my_dir.h>
#include "mysys_err.h"

#ifdef _WIN32
#define FIND_DATA_SIZE sizeof(WIN32_FIND_DATAA)
#else
#include <dirent.h>
#define FIND_DATA_SIZE 0
#endif


/*
  Check if a directory entry is the current or the parent directory.
*/

static my_bool is_dot_name(const char *name)
{
  return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}


/*
  Check if a character separates two parts of a path name.
*/

static my_bool is_dir_separator(char chr)
{
  return chr == FN_LIBCHAR || chr == FN_LIBCHAR2;
}


/*
  Check if a character separates a device name from a path name, like
  the ':' in "C:\data" on Windows.
*/

static my_bool is_device_char(char chr __attribute__((unused)))
{
#ifdef FN_DEVCHAR
  return chr == FN_DEVCHAR;
#else
  return 0;
#endif
}


/*
  Check if an entry should be given to the caller.

  @param flags  MY_DIR_ONLY_FILES and/or MY_DIR_ONLY_DIRS
  @param mode   the MY_S_IFMT part of the entry mode
*/

static my_bool entry_wanted(myf flags, uint mode)
{
  if (flags & MY_DIR_ONLY_FILES)
    return MY_S_ISREG(mode) != 0;
  if (flags & MY_DIR_ONLY_DIRS)
    return MY_S_ISDIR(mode) != 0;
  return 1;
}


/*
  Store the name of an entry in the buffer given by the caller.
  The buffer is always null terminated.

  @return MY_DIR_OK or MY_DIR_NAME_TOO_LONG
*/

static int store_entry_name(char *path, size_t path_length,
                            const char *name, size_t name_length,
                            const char *dir_name, myf MyFlags)
{
  DBUG_ASSERT(path_length > 0);
  if (unlikely(name_length >= path_length))
  {
    memcpy(path, name, path_length - 1);
    path[path_length - 1]= 0;
    my_errno= errno= ENAMETOOLONG;
    /*
      With ME_WARNING the caller wants to skip the file and continue,
      so the message is given as a warning and not as an error.
    */
    if (MyFlags & (MY_FAE | MY_WME | ME_WARNING))
      my_error(EE_DIR, MYF((MyFlags & (ME_WARNING | ME_NOTE | ME_ERROR_LOG))),
               dir_name, my_errno);
    return MY_DIR_NAME_TOO_LONG;
  }
  memcpy(path, name, name_length + 1);
  return MY_DIR_OK;
}


/*
  Store what we know about an entry without calling stat().
*/

static void store_mode(MY_STAT *stat_area, uint mode)
{
  bzero((char*) stat_area, sizeof(*stat_area));
  stat_area->st_mode= mode;
}


/*
  Build the name of a file in the directory.

  @return 0 on success
*/

static int dir_file_name(MY_NO_CACHE_DIR *dir, char *buff, size_t buff_size,
                         const char *name, myf MyFlags)
{
  const char *separator=
    is_dir_separator(dir->path.str[dir->path.length - 1]) ? "" : FN_ROOTDIR;

  if (dir->path.length + strlen(separator) + strlen(name) >= buff_size)
  {
    my_errno= errno= ENAMETOOLONG;
    if (MyFlags & (MY_FAE | MY_WME | ME_WARNING))
      my_error(EE_DIR, MYF(MyFlags & (MY_FAE | MY_WME | ME_WARNING)),
               dir->path.str, my_errno);
    return 1;
  }
  strxmov(buff, dir->path.str, separator, name, NullS);
  return 0;
}


/*
  Create a MY_NO_CACHE_DIR and store the directory name and the filter
  in it. Any trailing directory separator is removed from the stored
  name, except for a root directory like "/" or "C:\".

  @return 0 in case of out of memory
*/

static MY_NO_CACHE_DIR *dir_alloc(const char *path, const char *filter,
                                  myf MyFlags)
{
  MY_NO_CACHE_DIR *dir;
  char *path_buff, *filter_buff, *find_data;
  size_t path_length, filter_length;

  if (!path[0])
    path= ".";                                  /* Use the current dir */
  path_length= strlen(path);
  while (path_length > 1 && is_dir_separator(path[path_length - 1]) &&
         !is_device_char(path[path_length - 2]))
    path_length--;
  filter_length= filter ? strlen(filter) + 1 : 0;

  if (!my_multi_malloc(key_memory_MY_DIR, MYF(MyFlags | MY_ZEROFILL),
                       &dir, (uint) sizeof(*dir),
                       &path_buff, (uint) (path_length + 1),
                       &filter_buff, (uint) filter_length,
                       &find_data, (uint) FIND_DATA_SIZE,
                       NullS))
    return 0;

  memcpy(path_buff, path, path_length);
  path_buff[path_length]= 0;
  dir->path.str= path_buff;
  dir->path.length= path_length;
  if (filter)
  {
    dir->filter.str= (char*) memcpy(filter_buff, filter, filter_length);
    dir->filter.length= filter_length - 1;
  }
  dir->find_data= find_data;
  /*
    Remember how the caller wants errors to be reported, so that
    my_dir_read_next() can use the same flags.
  */
  dir->flags= MyFlags & (MY_DIR_ONLY_FILES | MY_DIR_ONLY_DIRS |
                         MY_FAE | MY_WME | ME_WARNING | ME_NOTE);
  return dir;
}


#ifdef _WIN32

/*
  Convert a Windows file time to a time_t.
*/

static time_t filetime_to_time(const FILETIME *file_time)
{
  ULARGE_INTEGER large;
  large.LowPart=  file_time->dwLowDateTime;
  large.HighPart= file_time->dwHighDateTime;
  /* Convert 100 ns units since year 1601 to seconds since year 1970 */
  return (time_t) ((large.QuadPart - 116444736000000000ULL) / 10000000ULL);
}


/*
  Find out the type of an entry from the information that
  FindFirstFile() and FindNextFile() give us.
*/

static uint find_data_mode(const WIN32_FIND_DATAA *find_data)
{
  return (find_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ?
          MY_S_IFDIR : MY_S_IFREG);
}


/*
  Store what the operating system told us about an entry for free.
*/

static void store_entry_stat(MY_STAT *stat_area, uint mode,
                             const void *entry)
{
  const WIN32_FIND_DATAA *find_data= (const WIN32_FIND_DATAA*) entry;
  ULARGE_INTEGER size;

  if (!(find_data->dwFileAttributes & FILE_ATTRIBUTE_READONLY))
    mode|= MY_S_IWRITE;
  store_mode(stat_area, mode | MY_S_IREAD);
  size.LowPart=  find_data->nFileSizeLow;
  size.HighPart= find_data->nFileSizeHigh;
  stat_area->st_size=  size.QuadPart;
  stat_area->st_mtime= filetime_to_time(&find_data->ftLastWriteTime);
  stat_area->st_atime= filetime_to_time(&find_data->ftLastAccessTime);
  stat_area->st_ctime= filetime_to_time(&find_data->ftCreationTime);
}


/*
  Start a new search in dir->path. Used by my_dir_open() and
  my_dir_rewind(). The caller reports any error, based on errno.

  @return 0 on success
*/

static int find_first(MY_NO_CACHE_DIR *dir)
{
  char search_path[FN_REFLEN + 3];
  HANDLE handle;

  if (dir_file_name(dir, search_path, sizeof(search_path), "*", MYF(0)))
    return 1;

  handle= FindFirstFileA(search_path, (WIN32_FIND_DATAA*) dir->find_data);
  dir->dirp= (void*) handle;
  dir->pending= 0;
  if (handle != INVALID_HANDLE_VALUE)
  {
    dir->pending= 1;                    /* find_data is not used yet */
    return 0;
  }
  if (GetLastError() == ERROR_FILE_NOT_FOUND)
    return 0;                           /* Empty dir; read gives EOF */

  my_osmaperr(GetLastError());          /* Sets errno for the caller */
  return 1;
}


/*
  Close the directory handle. The caller reports any error.

  @return 0 on success
*/

static int close_dir(MY_NO_CACHE_DIR *dir)
{
  if ((HANDLE) dir->dirp == INVALID_HANDLE_VALUE)
    return 0;                           /* Empty dir; nothing to close */
  if (FindClose((HANDLE) dir->dirp))
    return 0;
  my_osmaperr(GetLastError());          /* Sets errno for the caller */
  return 1;
}

#else /* _WIN32 */

/*
  Find out the type of an entry from the information that readdir()
  gives us. Returns 0 if the type is not known and stat() has to be
  used.
*/

static uint dirent_mode(const struct dirent *entry)
{
#ifdef DT_UNKNOWN
  switch (entry->d_type) {
  case DT_REG:
    return MY_S_IFREG;
  case DT_DIR:
    return MY_S_IFDIR;
  case DT_LNK:                          /* Resolved with stat() */
  case DT_UNKNOWN:
    return 0;                           /* We have to use stat() */
  default:
    return MY_S_IFIFO;                  /* Not a file and not a dir */
  }
#else
  return 0;
#endif
}


/*
  Store what the operating system told us about an entry for free.
*/

static void store_entry_stat(MY_STAT *stat_area, uint mode,
                             const void *entry __attribute__((unused)))
{
  store_mode(stat_area, mode);
}


/*
  Close the directory handle. The caller reports any error.

  @return 0 on success
*/

static int close_dir(MY_NO_CACHE_DIR *dir)
{
  return closedir((DIR*) dir->dirp) != 0;
}

#endif /* _WIN32 */


/*
  Open a directory for traversal.

  @param path    directory to read
  @param filter  pattern with '*' and '?' that names have to match,
                 or NULL to get all names
  @param MyFlags MY_DIR_ONLY_FILES, MY_DIR_ONLY_DIRS, MY_WME, MY_FAE, ME_WARNING

  @return 0 in case of error
*/

MY_NO_CACHE_DIR *my_dir_open(const char *path, const char *filter,
                             myf MyFlags)
{
  MY_NO_CACHE_DIR *dir;
  my_bool res;
  DBUG_ENTER("my_dir_open");
  DBUG_PRINT("my", ("path: '%s'  filter: '%s'  MyFlags: %lu",
                    path, filter ? filter : "", MyFlags));

  if (!(dir= dir_alloc(path, filter, MyFlags)))
    DBUG_RETURN(0);

#ifdef _WIN32
  res= find_first(dir) != 0;
#else
  res= !(dir->dirp= opendir(dir->path.str));
#endif
  if (unlikely(res))
  {
    my_errno= errno;
    if (MyFlags & (MY_FAE | MY_WME | ME_WARNING))
      my_error(EE_DIR, MYF(MyFlags & (MY_FAE | MY_WME | ME_WARNING)),
               path, my_errno);
    my_free(dir);
    DBUG_RETURN(0);
  }
  DBUG_RETURN(dir);
}


/*
  Read the next matching entry in a directory.

  @param dir          directory opened by my_dir_open()
  @param path         buffer where the entry name is stored
  @param path_length  size of the buffer, including the end null
  @param stat_area    where to store information about the entry, or
                      NULL if the caller is not interested
  @param MyFlags      MY_WANT_STAT, MY_WME, MY_FAE, ME_WARNING, ME_NOTE

  @return MY_DIR_OK             name stored in path
  @return MY_DIR_EOF            no more entries
  @return MY_DIR_NAME_TOO_LONG  the name did not fit into path
*/

int my_dir_read_next(MY_NO_CACHE_DIR *dir, char *path, size_t path_length,
                     MY_STAT *stat_area, myf MyFlags)
{
  DBUG_ENTER("my_dir_read_next");

  MyFlags|= dir->flags & (MY_FAE | MY_WME | ME_WARNING | ME_NOTE);

  for (;;)
  {
    char full_path[FN_REFLEN + 1];
    const void *entry;
    const char *name;
    MY_STAT stat_buff;
    size_t name_length;
    uint mode;

#ifdef _WIN32
    WIN32_FIND_DATAA *find_data= (WIN32_FIND_DATAA*) dir->find_data;

    if ((HANDLE) dir->dirp == INVALID_HANDLE_VALUE)
      DBUG_RETURN(MY_DIR_EOF);          /* The directory was empty */
    if (dir->pending)
      dir->pending= 0;                  /* Use the FindFirstFile() entry */
    else if (!FindNextFileA((HANDLE) dir->dirp, find_data))
      DBUG_RETURN(MY_DIR_EOF);
    entry= find_data;
    name=  find_data->cFileName;
    mode=  find_data_mode(find_data);
    /*
      Do not show hidden and system files, which Windows sometimes
      creates. This is what my_dir() does.
    */
    if (find_data->dwFileAttributes &
        (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
      continue;
#else
    const struct dirent *dir_entry= readdir((DIR*) dir->dirp);

    if (!dir_entry)
      DBUG_RETURN(MY_DIR_EOF);
    entry= dir_entry;
    name=  dir_entry->d_name;
    mode=  dirent_mode(dir_entry);
#endif

    if (is_dot_name(name))
      continue;
    name_length= strlen(name);
    /*
      wild_compare() cannot be used here, as the server changes
      wild_many and wild_one to the SQL wildcards '%' and '_'.
    */
    if (dir->filter.str &&
        my_wildcmp_bin(&my_charset_bin, name, name + name_length,
                       dir->filter.str, dir->filter.str + dir->filter.length,
                       0, '?', '*'))
      continue;

    if (!mode || (stat_area && (MyFlags & MY_WANT_STAT)))
    {
      /*
        We have to use stat(); either the file system does not tell us
        the type of the entry, the entry is a symbolic link that has
        to be resolved or the caller wants a full stat.
      */
      if (dir_file_name(dir, full_path, sizeof(full_path), name, MyFlags))
        DBUG_RETURN(MY_DIR_NAME_TOO_LONG);
      if (!my_stat(full_path, &stat_buff, MYF(0)))
        continue;                       /* Entry was deleted */
      mode= stat_buff.st_mode & MY_S_IFMT;
      if (!entry_wanted(dir->flags, mode))
        continue;
      if (stat_area)
      {
        memcpy(stat_area, &stat_buff, sizeof(*stat_area));
        DBUG_RETURN(store_entry_name(path, path_length, name, name_length,
                                     dir->path.str, MyFlags));
      }
    }
    else if (!entry_wanted(dir->flags, mode))
      continue;

    if (stat_area)
      store_entry_stat(stat_area, mode, entry);
    DBUG_RETURN(store_entry_name(path, path_length, name, name_length,
                                 dir->path.str, MyFlags));
  }
}


/*
  Start reading the directory from the beginning.

  @param dir      directory opened by my_dir_open()
  @param MyFlags  MY_WME, MY_FAE, ME_WARNING

  @return 0 on success
*/

int my_dir_rewind(MY_NO_CACHE_DIR *dir, myf MyFlags)
{
  int error;
  DBUG_ENTER("my_dir_rewind");

#ifdef _WIN32
  error= close_dir(dir) | find_first(dir);
#else
  rewinddir((DIR*) dir->dirp);
  error= 0;
#endif
  if (unlikely(error))
  {
    myf flags= (MyFlags | dir->flags) & (MY_FAE | MY_WME | ME_WARNING);
    my_errno= errno;
    if (flags)
      my_error(EE_DIR, MYF(flags), dir->path.str, my_errno);
  }
  DBUG_RETURN(error);
}


/*
  Close a directory opened by my_dir_open().

  @return 0 on success
*/

int my_dir_close(MY_NO_CACHE_DIR *dir)
{
  int error;
  DBUG_ENTER("my_dir_close");

  if ((error= close_dir(dir)))
    my_errno= errno;
  my_free(dir);
  DBUG_RETURN(error);
}
