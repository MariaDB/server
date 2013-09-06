#ifndef MYSQL_SERVICE_THD_TIMEZONE_INCLUDED
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
  This service provdes functions to convert between my_time_t and
  MYSQL_TIME taking into account the current value of the time_zone
  session variable.

  The values of the my_time_t type are in Unix timestamp format,
  i.e. the number of seconds since "1970-01-01 00:00:00 UTC".

  The values of the MYSQL_TIME type are in the current time zone,
  according to thd->variables.time_zone.

  If the MYSQL_THD parameter is NULL, then global_system_variables.time_zone
  is used for conversion.
*/

#ifndef MYSQL_ABI_CHECK
/*
  This service currently does not depend on any system headers.
  If it needs system headers in the future, make sure to put
  them inside this ifndef.
*/
#endif

#include "mysql_time.h"

#ifdef __cplusplus
extern "C" {
#endif


extern struct thd_timezone_service_st {
  my_time_t (*thd_TIME_to_gmt_sec)(MYSQL_THD thd, const MYSQL_TIME *ltime, unsigned int *errcode);
  void (*thd_gmt_sec_to_TIME)(MYSQL_THD thd, MYSQL_TIME *ltime, my_time_t t);
} *thd_timezone_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define thd_TIME_to_gmt_sec(thd, ltime, errcode) \
  (thd_timezone_service->thd_TIME_to_gmt_sec((thd), (ltime), (errcode)))

#define thd_gmt_sec_to_TIME(thd, ltime, t) \
  (thd_timezone_service->thd_gmt_sec_to_TIME((thd), (ltime), (t)))

#else

my_time_t thd_TIME_to_gmt_sec(MYSQL_THD thd, const MYSQL_TIME *ltime, unsigned int *errcode);
void thd_gmt_sec_to_TIME(MYSQL_THD thd, MYSQL_TIME *ltime, my_time_t t);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_THD_TIMEZONE_INCLUDED
#endif
