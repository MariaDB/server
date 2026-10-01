/* Copyright (c) 2001, 2010, Oracle and/or its affiliates. All rights reserved.
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

/*
  Result data of a connection that is served by the same process, as in
  Protocol_local (sql_prepare.cc), which is behind the SQL service.

  The name is historical: it came from the embedded server library
  that does not exist any more (MDEV-11111), whose "embedded" connections
  were of this kind.
*/

#include <sql_common.h>

C_MODE_START

/* Collects the data that the server returns to such a connection */
typedef struct embedded_query_result
{
  MYSQL_ROWS **prev_ptr;
  unsigned int warning_count, server_status;
  struct st_mysql_data *next;
  my_ulonglong affected_rows, insert_id;
  char info[MYSQL_ERRMSG_SIZE];
  MYSQL_FIELD *fields_list;
  unsigned int last_errno;
  char sqlstate[SQLSTATE_LENGTH+1];
} EQR;


typedef struct st_mariadb_field_extension
{
  MARIADB_CONST_STRING metadata[MARIADB_FIELD_ATTR_LAST+1]; /* 10.5 */
} MARIADB_FIELD_EXTENSION;

C_MODE_END
