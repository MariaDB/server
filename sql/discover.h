/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef DISCOVER_INCLUDED
#define DISCOVER_INCLUDED

int extension_based_table_discovery(MY_DIR *dirp, const char *ext,
                                    handlerton::discovered_list *tl);

#ifdef MYSQL_SERVER
int readfrm(const char *name, const uchar **data, size_t *length);
int writefile(const char *path, const char *db, const char *table,
              bool tmp_table, const uchar *frmdata, size_t len);

/* a helper to delete an frm file, given a path w/o .frm extension */
inline void deletefrm(const char *path)
{
  char frm_name[FN_REFLEN];
  strxnmov(frm_name, sizeof(frm_name)-1, path, reg_ext, NullS);
  mysql_file_delete(key_file_frm, frm_name, MYF(0));
}

int ext_table_discovery_simple(MY_DIR *dirp,
                               handlerton::discovered_list *result);
#endif

#endif /* DISCOVER_INCLUDED */
