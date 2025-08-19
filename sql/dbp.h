/* Copyright (c) 2026, MariaDB

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

#ifndef __DBP_H__
#define __DBP_H__

#include <sys/types.h>
#include "my_global.h"
#include "my_json_writer.h"

#define DBUG_PRINT_FUNCTION dbp
#define DBUG_PRINT_ROW  dbp_row
#define DBUG_PRINT_TRACE  dbp_trace
#define DBUG_ITEM_BUFFER_SIZE 2048
extern void dbug_strlcat( char *dest, uint dest_size, const char *src);

extern void dbug_sprintfcat(char *dest, uint dest_size, const char *format, ...);
extern const char *dbug_add_print_field(Field *field);

#define DBUG_CAT(...) dbug_strlcat(dbug_big_buffer, DBUG_BIG_BUFFER_SIZE, \
                                   __VA_ARGS__)

#define DBUG_SPRINTF_CAT(...) dbug_sprintfcat(dbug_big_buffer,\
                                DBUG_BIG_BUFFER_SIZE, __VA_ARGS__)

#define DBUG_TRASH_CHAR      0xa5

#define DBUG_BIG_BUFFER_SIZE 20480
static char dbug_big_buffer[DBUG_BIG_BUFFER_SIZE];

#define DBUG_ROW_BUFFER_SIZE 1024

#endif
