/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include "duckdb_types.h"
#include <stddef.h>
#include <cassert>
#include <cstring>

#include <my_global.h>
#include "m_string.h"
#include "sql_table.h"

/* Refers to create_table_info_t::normalize_table_name */
DatabaseTableNames::DatabaseTableNames(const char *name)
{
  const char *name_ptr;
  size_t name_len;
  const char *db_ptr;
  size_t db_len;
  const char *ptr;

  /* Scan name from the end */
  ptr= strend(name) - 1;

  /* seek to the last path separator */
  while (ptr >= name && *ptr != '\\' && *ptr != '/')
    ptr--;

  name_ptr= ptr + 1;
  name_len= strlen(name_ptr);

  /* skip any number of path separators */
  while (ptr >= name && (*ptr == '\\' || *ptr == '/'))
    ptr--;

  assert(ptr >= name);

  /* seek to the last but one path separator */
  db_len= 0;
  while (ptr >= name && *ptr != '\\' && *ptr != '/')
  {
    ptr--;
    db_len++;
  }

  db_ptr= ptr + 1;

  std::string raw_table_name= std::string(name_ptr, name_len);
  std::string raw_db_name= std::string(db_ptr, db_len);

  /*
    When there are escape characters in the table name or database name
    (such as '-' is converted to '@002d'), we need to restore it to the
    original characters.

    Skip conversion for #sql- prefixed names (internal temp tables used
    during ALTER TABLE). filename_to_tablename() cannot handle the '#'
    prefix and would mangle the name. In AliSQL this is not an issue
    because dd::Table provides the clean name directly.
  */
  if (strncmp(raw_table_name.c_str(), "#sql", 4) == 0)
  {
    table_name= raw_table_name;
  }
  else
  {
    char ori_table_name[NAME_LEN + 1];
    size_t tbl_name_length= filename_to_tablename(
        raw_table_name.c_str(), ori_table_name, sizeof(ori_table_name));
    table_name= std::string(ori_table_name, tbl_name_length);
  }

  char ori_db_name[NAME_LEN + 1];
  size_t db_name_length= filename_to_tablename(
      raw_db_name.c_str(), ori_db_name, sizeof(ori_db_name));
  db_name= std::string(ori_db_name, db_name_length);
}

Databasename::Databasename(const char *path_name)
{
  char dbname[FN_REFLEN];
  const char *end, *ptr;
  char tmp_buff[FN_REFLEN + 1];

  char *tmp_name= tmp_buff;
  /* Scan name from the end */
  ptr= strend(path_name) - 1;
  while (ptr >= path_name && *ptr != '\\' && *ptr != '/')
    ptr--;
  ptr--;
  end= ptr;
  while (ptr >= path_name && *ptr != '\\' && *ptr != '/')
    ptr--;
  uint name_len= (uint) (end - ptr);
  memcpy(tmp_name, ptr + 1, name_len);
  tmp_name[name_len]= '\0';

  filename_to_tablename(tmp_name, dbname, sizeof(tmp_buff) - 1);
  name= std::string(dbname);
}
