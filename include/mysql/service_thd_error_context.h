#ifndef MYSQL_SERVICE_THD_STMT_DA_INCLUDED
/* Copyright (C) 2013 MariaDB Foundation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file
  This service provides access to the statement diagnostics area:
  - error message
  - error number
  - row for warning (e.g. for multi-row INSERT statements)
*/

#ifdef __cplusplus
extern "C" {
#endif


extern struct thd_error_context_service_st {
  const char *(*thd_get_error_message_func)(const MYSQL_THD thd);
  unsigned int (*thd_get_error_number_func)(const MYSQL_THD thd);
  unsigned long (*thd_get_error_row_func)(const MYSQL_THD thd);
  void (*thd_inc_error_row_func)(MYSQL_THD thd);
  char *(*thd_get_error_context_description_func)(MYSQL_THD thd,
                                                  char *buffer,
                                                  unsigned int length,
                                                  unsigned int max_query_length);
} *thd_error_context_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
#define thd_get_error_message(thd) \
  (thd_error_context_service->thd_get_error_message_func((thd)))
#define thd_get_error_number(thd) \
  (thd_error_context_service->thd_get_error_number_func((thd)))
#define thd_get_error_row(thd) \
  (thd_error_context_service->thd_get_error_row_func((thd)))
#define thd_inc_error_row(thd) \
  (thd_error_context_service->thd_inc_error_row_func((thd)))
#define thd_get_error_context_description(thd, buffer, length, max_query_len) \
  (thd_error_context_service->thd_get_error_context_description_func((thd), \
                                                                (buffer), \
                                                                (length), \
                                                                (max_query_len)))
#else
/**
  Return error message
  @param thd   user thread connection handle
  @return      error text
*/
const char *thd_get_error_message(const MYSQL_THD thd);
/**
  Return error number
  @param thd   user thread connection handle
  @return      error number
*/
unsigned int thd_get_error_number(const MYSQL_THD thd);
/**
  Return the current row number (i.e. in a multiple INSERT statement)
  @param thd   user thread connection handle
  @return      row number
*/
unsigned long thd_get_error_row(const MYSQL_THD thd);
/**
  Increment the current row number
  @param thd   user thread connection handle
*/
void thd_inc_error_row(MYSQL_THD thd);
/**
  Return a text description of a thread, its security context (user,host)
  and the current query.
*/
char *thd_get_error_context_description(MYSQL_THD thd,
                                        char *buffer, unsigned int length,
                                        unsigned int max_query_length);
#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_THD_STMT_DA_INCLUDED
#endif
