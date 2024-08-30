/* Copyright (c) 2019, MariaDB Corporation.

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

#pragma once

/**
  @file include/mysql/service_print_check_msg.h
  This service provides functions to write messages for check or repair
*/

#ifdef __cplusplus
extern "C" {
#endif


extern struct print_check_msg_service_st {
  void (*print_check_msg)(MYSQL_THD, const char *db_name, const char *table_name,
                          const char *op, const char *msg_type, const char *message,
                          my_bool print_to_log);
} *print_check_msg_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
# define print_check_msg_context(_THD) print_check_msg_service->print_check_msg
#else
extern void print_check_msg(MYSQL_THD, const char *db_name, const char *table_name,
                            const char *op, const char *msg_type, const char *message,
                            my_bool print_to_log);
#endif

#ifdef __cplusplus
}
#endif
