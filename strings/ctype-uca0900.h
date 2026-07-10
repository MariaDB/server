#ifndef CTYPE_UCA_0900_H
#define CTYPE_UCA_0900_H
/* Copyright (c) 2025, MariaDB Corporation

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */


#define mysql_0900_collation_start 255
#define mysql_0900_collation_end 308
#define mysql_0900_collation_num \
 (mysql_0900_collation_end - mysql_0900_collation_start + 1 + 1/*End marker*/)

struct mysql_0900_to_mariadb_1400_mapping
{
  const char *mysql_col_name, *mariadb_col_name, *case_sensitivity;
  uint collation_id;
};

extern struct mysql_0900_to_mariadb_1400_mapping
  mysql_0900_mapping[mysql_0900_collation_num];


static inline
my_bool my_collation_id_is_mysql_uca0900(uint id)
{
  return id >= mysql_0900_collation_start &&
         id <= mysql_0900_collation_end;
}

my_bool mysql_uca0900_utf8mb4_collation_definitions_add(MY_CHARSET_LOADER *ld);

my_bool mysql_utf8mb4_0900_bin_add(MY_CHARSET_LOADER *loader);

#endif /* CTYPE_UCA_0900_H */
