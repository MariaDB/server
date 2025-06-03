/* Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2022, MariaDB Corporation.

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


/**
  @file

  @brief
  Functions for discover of frm file from handler
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "discover.h"
#include <my_dir.h>

/**
  Read the contents of a .frm file.

  frmdata and len are set to 0 on error.

  @param name           path to table-file "db/name"
  @param frmdata        frm data
  @param len            length of the read frmdata

  @retval
    0	ok
  @retval
    1	Could not open file
  @retval
    2    Could not stat file
  @retval
    3    Could not allocate data for read.  Could not read file
*/

int readfrm(const char *name, const uchar **frmdata, size_t *len)
{
  int    error;
  char	 index_file[FN_REFLEN];
  File	 file;
  size_t read_len;
  uchar *read_data;
  MY_STAT state;  
  DBUG_ENTER("readfrm");
  DBUG_PRINT("enter",("name: '%s'",name));
  
  *frmdata= NULL;      // In case of errors
  *len= 0;
  error= 1;
  if ((file= mysql_file_open(key_file_frm,
                             fn_format(index_file, name, "", reg_ext,
                               MY_UNPACK_FILENAME|MY_APPEND_EXT),
                             O_RDONLY | O_SHARE,
                             MYF(0))) < 0)
    goto err_end; 
  
  // Get length of file
  error= 2;
  if (mysql_file_fstat(file, &state, MYF(0)))
    goto err;
  MSAN_STAT_WORKAROUND(&state);
  read_len= (size_t)MY_MIN(FRM_MAX_SIZE, state.st_size); // safety

  // Read whole frm file
  error= 3;
  if (!(read_data= (uchar*)my_malloc(key_memory_frm_string, read_len,
                                     MYF(MY_WME))))
    goto err;
  if (mysql_file_read(file, read_data, read_len, MYF(MY_NABP)))
  {
    my_free(read_data);
    goto err;
  }

  // Setup return data
  *frmdata= (uchar*) read_data;
  *len= read_len;
  error= 0;
  
 err:
  (void) mysql_file_close(file, MYF(MY_WME));
  
 err_end:		      /* Here when no file */
  DBUG_RETURN (error);
} /* readfrm */


/*
  Write the content of a frm data pointer to a frm or par file.

  @param path           full path to table-file "db/name.frm" or .par
  @param db             Database name. Only used for my_error()
  @param table          Table name. Only used for my_error()
  @param data           data to write to file
  @param len            length of the data

  @retval
    0	ok
  @retval
    <> 0    Could not write file. In this case the file is not created
*/

int writefile(const char *path, const char *db, const char *table,
              bool tmp_table, const uchar *data, size_t len)
{
  int error;
  int create_flags= O_RDWR | O_TRUNC;
  DBUG_ENTER("writefile");
  DBUG_PRINT("enter",("name: '%s' len: %lu ",path, (ulong) len));

  if (tmp_table)
    create_flags|= O_EXCL | O_NOFOLLOW;

  File file= mysql_file_create(key_file_frm, path,
                               CREATE_MODE, create_flags, MYF(0));

  if (unlikely((error= file < 0)))
  {
    if (my_errno == ENOENT)
      my_error(ER_BAD_DB_ERROR, MYF(0), db);
    else
      my_error(ER_CANT_CREATE_TABLE, MYF(0), db, table, my_errno);
  }
  else
  {
    error= (int)mysql_file_write(file, data, len, MYF(MY_WME | MY_NABP));

    if (!error && !tmp_table && opt_sync_frm)
        error= mysql_file_sync(file, MYF(MY_WME)) ||
             my_sync_dir_by_file(path, MYF(MY_WME));

    error|= mysql_file_close(file, MYF(MY_WME));
    if (error)
      my_delete(path, MYF(0));
  }
  DBUG_RETURN(error);
} /* writefile */


static inline void advance(FILEINFO* &from, FILEINFO* &to,
                           FILEINFO* cur, bool &skip)
{
  if (skip)                   // if not copying
    from= cur;                //   just advance the start pointer
  else                        // if copying
    if (to == from)           //   but to the same place, not shifting the data
      from= to= cur;          //     advance both pointers
    else                      //   otherwise
      while (from < cur)      //     have to copy [from...cur) to [to...)
        *to++ = *from++;
  skip= false;
}

/**
  Go through the directory listing looking for files with a specified
  extension and add them to the result list

  @details
  This function may be called many times on the same directory listing
  but with different extensions. To avoid discovering the same table twice,
  whenever a table file is discovered, all files with the same name
  (independently from the extensions) are removed from the list.

  Example: the list contained
     { "db.opt", "t1.MYD", "t1.MYI", "t1.frm", "t2.ARZ", "t3.ARZ", "t3.frm" }
  on discovering all ".frm" files, tables "t1" and "t3" will be found,
  and list will become
     { "db.opt", "t2.ARZ" }
  and now ".ARZ" discovery can discover the table "t2"

  @note
  This function assumes that the directory listing is sorted alphabetically.

  @note  Partitioning makes this more complicated. A partitioned table t1 might
  have files, like t1.frm, t1#P#part1.ibd, t1#P#foo.ibd, etc.
  That means we need to compare file names only up to the first '#' or '.'
  whichever comes first.
*/
int extension_based_table_discovery(MY_DIR *dirp, const char *ext_meta,
                                    handlerton::discovered_list *result)
{
  CHARSET_INFO *cs= character_set_filesystem;
  size_t ext_meta_len= strlen(ext_meta);
  FILEINFO *from, *to, *cur, *end;
  bool skip= false;
  
  from= to= cur= dirp->dir_entry;
  end= cur + dirp->number_of_files;
  while (cur < end)
  {
    char *octothorp= strchr(cur->name + 1, '#');
    char *ext= strchr(octothorp ? octothorp : cur->name, FN_EXTCHAR);

    if (ext)
    {
      size_t len= (octothorp ? octothorp : ext) - cur->name;
      if (from != cur &&
          (strlen(from->name) <= len ||
           cs->strnncoll(from->name, len, cur->name, len) ||
           (from->name[len] != FN_EXTCHAR && from->name[len] != '#')))
        advance(from, to, cur, skip);

      if (cs->strnncoll(ext, strlen(ext),
                        ext_meta, ext_meta_len) == 0)
      {
        *ext = 0;
        if (result->add_file(cur->name))
          return 1;
        *ext = FN_EXTCHAR;
        skip= true; // table discovered, skip all files with the same name
      }
    }
    else
    {
      advance(from, to, cur, skip);
      from++;
    }

    cur++;
  }
  advance(from, to, cur, skip);
  dirp->number_of_files= to - dirp->dir_entry;
  return 0;
}

/**
  Simple, not reusable file-based table discovery

  @details
  simplified version of extension_based_table_discovery(), that does not
  modify the list of files. It cannot be called many times for the same
  directory listing, otherwise it'll produce duplicate results.
*/
int ext_table_discovery_simple(MY_DIR *dirp,
                               handlerton::discovered_list *result)
{
  CHARSET_INFO *cs= character_set_filesystem;
  FILEINFO *cur, *end;
  
  cur= dirp->dir_entry;
  end= cur + dirp->number_of_files;
  while (cur < end)
  {
    char *ext= strrchr(cur->name, FN_EXTCHAR);

    if (ext)
    {
      if (cs->strnncoll(ext, strlen(ext),
                        reg_ext, reg_ext_length) == 0)
      {
        *ext = 0;
        if (result->add_file(cur->name))
          return 1;
      }
    }
    cur++;
  }
  return 0;
}

