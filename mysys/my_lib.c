/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2008, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA.
*/

/* TODO: check for overrun of memory for names. */

#include	"mysys_priv.h"
#include	<m_string.h>
#include	<my_dir.h>	/* Structs used by my_dir,includes sys/types */
#include	"mysys_err.h"
#if defined(HAVE_DIRENT_H)
# include <dirent.h>
#else
# define dirent direct
# if defined(HAVE_SYS_NDIR_H)
#  include <sys/ndir.h>
# endif
# if defined(HAVE_SYS_DIR_H)
#  include <sys/dir.h>
# endif
# if defined(HAVE_NDIR_H)
#  include <ndir.h>
# endif
# if defined(_WIN32)
# ifdef __BORLANDC__
# include <dir.h>
# endif
# endif
#endif

#if defined(HAVE_READDIR_R)
#define READDIR(A,B,C) ((errno=readdir_r(A,B,&C)) != 0 || !C)
#else
#define READDIR(A,B,C) (!(C=readdir(A)))
#endif

/*
  We are assuming that directory we are reading is either has less than 
  100 files and so can be read in one initial chunk or has more than 1000
  files and so big increment are suitable.
*/
#define ENTRIES_START_SIZE (8192/sizeof(FILEINFO))
#define ENTRIES_INCREMENT  (65536/sizeof(FILEINFO))
#define NAMES_START_SIZE   32768


static int	comp_names(struct fileinfo *a,struct fileinfo *b);

typedef struct {
  MY_DIR        dir;
  DYNAMIC_ARRAY array;
  MEM_ROOT      root;
} MY_DIR_HANDLE;

/* We need this because the caller doesn't know which malloc we've used */

void my_dirend(MY_DIR *dir)
{
  MY_DIR_HANDLE *dirh= (MY_DIR_HANDLE*) dir;
  DBUG_ENTER("my_dirend");
  if (dirh)
  {
    delete_dynamic(&dirh->array);
    free_root(&dirh->root, MYF(0));
    my_free(dirh);
  }
  DBUG_VOID_RETURN;
} /* my_dirend */


        /* Compare in sort of filenames */

static int comp_names(struct fileinfo *a, struct fileinfo *b)
{
  return (strcmp(a->name,b->name));
} /* comp_names */


#if !defined(_WIN32)

static char *directory_file_name (char * dst, const char *src)
{
  /* Process as Unix format: just remove test the final slash. */
  char *end;
  DBUG_ASSERT(strlen(src) < (FN_REFLEN + 1));

  if (src[0] == 0)
    src= (char*) ".";                           /* Use empty as current */
  end= strnmov(dst, src, FN_REFLEN + 1);
  if (end[-1] != FN_LIBCHAR)
  {
    *end++= FN_LIBCHAR;                          /* Add last '/' */
    *end='\0';
  }
  return end;
}

MY_DIR	*my_dir(const char *path, myf MyFlags)
{
  MY_DIR_HANDLE *dirh;
  FILEINFO      finfo;
  DIR		*dirp;
  struct dirent *dp;
  char		tmp_path[FN_REFLEN + 2], *tmp_file;
  char	dirent_tmp[sizeof(struct dirent)+_POSIX_PATH_MAX+1];

  DBUG_ENTER("my_dir");
  DBUG_PRINT("my",("path: '%s' MyFlags: %lu",path,MyFlags));

  tmp_file= directory_file_name(tmp_path, path);

  if (!(dirp= opendir(tmp_path)))
  {
    my_errno= errno;
    goto err_open;
  }

  if (!(dirh= my_malloc(sizeof(*dirh), MyFlags | MY_ZEROFILL)))
    goto err_alloc;
  
  if (my_init_dynamic_array(&dirh->array, sizeof(FILEINFO),
                            ENTRIES_START_SIZE, ENTRIES_INCREMENT,
                            MYF(MyFlags)))
    goto error;
  
  init_alloc_root(&dirh->root, NAMES_START_SIZE, NAMES_START_SIZE,
                  MYF(MyFlags));

  dp= (struct dirent*) dirent_tmp;
  
  while (!(READDIR(dirp,(struct dirent*) dirent_tmp,dp)))
  {
    MY_STAT statbuf, *mystat= 0;
    
    if (dp->d_name[0] == '.' &&
        (dp->d_name[1] == '\0' ||
         (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))
      continue;                               /* . or .. */

    if (MyFlags & MY_WANT_STAT)
    {
      mystat= &statbuf;
      bzero(mystat, sizeof(*mystat));
      (void) strmov(tmp_file, dp->d_name);
      (void) my_stat(tmp_path, mystat, MyFlags);
      if (!(mystat->st_mode & MY_S_IREAD))
        continue;
    }

    if (!(finfo.name= strdup_root(&dirh->root, dp->d_name)))
      goto error;
    
    if (mystat &&
        !((mystat= memdup_root(&dirh->root, mystat, sizeof(*mystat)))))
      goto error;

    finfo.mystat= mystat;

    if (push_dynamic(&dirh->array, (uchar*)&finfo))
      goto error;
  }

  (void) closedir(dirp);
  
  if (MyFlags & MY_WANT_SORT)
    sort_dynamic(&dirh->array, (qsort_cmp) comp_names);

  dirh->dir.dir_entry= dynamic_element(&dirh->array, 0, FILEINFO *);
  dirh->dir.number_of_files= dirh->array.elements;
  
  DBUG_RETURN(&dirh->dir);

error:
  my_dirend(&dirh->dir);
err_alloc:
  (void) closedir(dirp);
err_open:
  if (MyFlags & (MY_FAE | MY_WME))
    my_error(EE_DIR, MYF(ME_BELL | ME_WAITTANG), path, my_errno);
  DBUG_RETURN(NULL);
} /* my_dir */


#else

/*
*****************************************************************************
** Read long filename using windows rutines
*****************************************************************************
*/

MY_DIR	*my_dir(const char *path, myf MyFlags)
{
  MY_DIR_HANDLE *dirh= 0;
  FILEINFO      finfo;
  struct _finddata_t find;
  ushort	mode;
  char		tmp_path[FN_REFLEN], *tmp_file,attrib;
#ifdef _WIN64
  __int64       handle;
#else
  long		handle;
#endif
  DBUG_ENTER("my_dir");
  DBUG_PRINT("my",("path: '%s' MyFlags: %d",path,MyFlags));

  /* Put LIB-CHAR as last path-character if not there */
  tmp_file=tmp_path;
  if (!*path)
    *tmp_file++ ='.';				/* From current dir */
  tmp_file= strnmov(tmp_file, path, FN_REFLEN-5);
  if (tmp_file[-1] == FN_DEVCHAR)
    *tmp_file++= '.';				/* From current dev-dir */
  if (tmp_file[-1] != FN_LIBCHAR)
    *tmp_file++ =FN_LIBCHAR;
  tmp_file[0]='*';				/* Windows needs this !??? */
  tmp_file[1]='.';
  tmp_file[2]='*';
  tmp_file[3]='\0';

  if (!(dirh= my_malloc(sizeof(*dirh), MyFlags | MY_ZEROFILL)))
    goto error;
  
  if (my_init_dynamic_array(&dirh->array, sizeof(FILEINFO),
                            ENTRIES_START_SIZE, ENTRIES_INCREMENT,
                            MYF(MyFlags)))
    goto error;

  init_alloc_root(&dirh->root, NAMES_START_SIZE, NAMES_START_SIZE,
                  MYF(MyFlags));

  if ((handle=_findfirst(tmp_path,&find)) == -1L)
  {
    DBUG_PRINT("info", ("findfirst returned error, errno: %d", errno));
    if  (errno != EINVAL)
      goto error;
    /*
      Could not read the directory, no read access.
      Probably because by "chmod -r".
      continue and return zero files in dir
    */
  }
  else
  {
    do
    {
      attrib= find.attrib;
      /*
        Do not show hidden and system files which Windows sometimes create.
        Note. Because Borland's findfirst() is called with the third
        argument = 0 hidden/system files are excluded from the search.
      */
      if (attrib & (_A_HIDDEN | _A_SYSTEM))
        continue;

      if (find.name[0] == '.' &&
          (find.name[1] == '\0' ||
           (find.name[1] == '.' && find.name[2] == '\0')))
        continue;                               /* . or .. */

      if (!(finfo.name= strdup_root(&dirh->root, find.name)))
        goto error;
      if (MyFlags & MY_WANT_STAT)
      {
        if (!(finfo.mystat= (MY_STAT*)alloc_root(&dirh->root, sizeof(MY_STAT))))
          goto error;

        bzero(finfo.mystat, sizeof(MY_STAT));
        finfo.mystat->st_size=find.size;
        mode= MY_S_IREAD;
        if (!(attrib & _A_RDONLY))
          mode|= MY_S_IWRITE;
        if (attrib & _A_SUBDIR)
          mode|= MY_S_IFDIR;
        finfo.mystat->st_mode= mode;
        finfo.mystat->st_mtime= ((uint32) find.time_write);
      }
      else
        finfo.mystat= NULL;

      if (push_dynamic(&dirh->array, (uchar*)&finfo))
        goto error;
    }
    while (_findnext(handle,&find) == 0);
    _findclose(handle);
  }

  if (MyFlags & MY_WANT_SORT)
    sort_dynamic(&dirh->array, (qsort_cmp) comp_names);

  dirh->dir.dir_entry= dynamic_element(&dirh->array, 0, FILEINFO *);
  dirh->dir.number_of_files= dirh->array.elements;

  DBUG_PRINT("exit", ("found %d files", dirh->dir.number_of_files));
  DBUG_RETURN(&dirh->dir);
error:
  my_errno=errno;
  if (handle != -1)
      _findclose(handle);
  my_dirend(&dirh->dir);
  if (MyFlags & (MY_FAE | MY_WME))
    my_error(EE_DIR,MYF(ME_BELL | ME_WAITTANG), path, errno);
  DBUG_RETURN(NULL);
} /* my_dir */

#endif /* _WIN32 */

/****************************************************************************
** File status
** Note that MY_STAT is assumed to be same as struct stat
****************************************************************************/ 


int my_fstat(File Filedes, MY_STAT *stat_area,
             myf MyFlags __attribute__((unused)))
{
  DBUG_ENTER("my_fstat");
  DBUG_PRINT("my",("fd: %d  MyFlags: %lu", Filedes, MyFlags));
#ifdef _WIN32
  DBUG_RETURN(my_win_fstat(Filedes, stat_area));
#else
  DBUG_RETURN(fstat(Filedes, (struct stat *) stat_area));
#endif
}


MY_STAT *my_stat(const char *path, MY_STAT *stat_area, myf my_flags)
{
  int m_used;
  DBUG_ENTER("my_stat");
  DBUG_PRINT("my", ("path: '%s'  stat_area: %p  MyFlags: %lu", path,
                    stat_area, my_flags));

  if ((m_used= (stat_area == NULL)))
    if (!(stat_area= (MY_STAT *) my_malloc(sizeof(MY_STAT), my_flags)))
      goto error;
#ifndef _WIN32
    if (! stat((char *) path, (struct stat *) stat_area) )
      DBUG_RETURN(stat_area);
#else
    if (! my_win_stat(path, stat_area) )
      DBUG_RETURN(stat_area);
#endif
  DBUG_PRINT("error",("Got errno: %d from stat", errno));
  my_errno= errno;
  if (m_used)					/* Free if new area */
    my_free(stat_area);

error:
  if (my_flags & (MY_FAE+MY_WME))
  {
    my_error(EE_STAT, MYF(ME_BELL+ME_WAITTANG),path,my_errno);
    DBUG_RETURN((MY_STAT *) NULL);
  }
  DBUG_RETURN((MY_STAT *) NULL);
} /* my_stat */
