/* Copyright (c) 2016, MariaDB

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

#ifndef MYSQL_SERVICE_MY_PRINT_ERROR_INCLUDED
#define MYSQL_SERVICE_MY_PRINT_ERROR_INCLUDED

/**
  @file include/mysql/service_my_print_error.h

  This service provides functions for plugins to report
  errors to client (without client, the errors are written to the error log).

*/
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdarg.h>
#include <stdlib.h>
#endif

#define ME_ERROR_LOG        64 /* Write the message to the error log */
#define ME_ERROR_LOG_ONLY  128 /* Write the error message to error log only */
#define ME_NOTE           1024 /* Not an error, just a note */
#define ME_WARNING        2048 /* Not an error, just a warning */
#define ME_FATAL          4096 /* Fatal statement error */

extern struct my_print_error_service_st {
  void (*my_error_func)(unsigned int nr, unsigned long MyFlags, ...);
  void (*my_printf_error_func)(unsigned int nr, const char *fmt, unsigned long MyFlags,...);
  void (*my_printv_error_func)(unsigned int error, const char *format, unsigned long MyFlags, va_list ap);
} *my_print_error_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define my_error my_print_error_service->my_error_func
#define my_printf_error my_print_error_service->my_printf_error_func
#define my_printv_error(A,B,C,D) my_print_error_service->my_printv_error_func(A,B,C,D)

#else
extern void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern void my_printf_error(unsigned int my_err, const char *format, unsigned long MyFlags, ...);
extern void my_printv_error(unsigned int error, const char *format, unsigned long MyFlags,va_list ap);
#endif /* MYSQL_DYNAMIC_PLUGIN */

#ifdef __cplusplus
}
#endif

#endif

