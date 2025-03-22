#ifndef CTYPE_UCA_0900_H
#define CTYPE_UCA_0900_H
/* Copyright (c) 2025, MariaDB

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

struct mysql_0900_to_mariadb_1400_mapping
{
  const char *mysql_col_name, *mariadb_col_name, *case_sensitivity;
  uint collation_id;
};

extern struct mysql_0900_to_mariadb_1400_mapping mysql_0900_mapping[];


LEX_CSTRING
my_uca0900_collation_build_name(char *buffer, size_t buffer_size,
                                const char *cs_name,
                                const char *tailoring_name,
                                const char *sensitivity_suffix);

#endif /* CTYPE_UCA_0900_H */
