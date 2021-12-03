#ifndef MYSQL_SERVICE_THD_AUTOINC_INCLUDED
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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file
  This service provides access to the auto_increment related system variables:

  @@auto_increment_offset
  @@auto_increment_increment
*/

#ifdef __cplusplus
extern "C" {
#endif

extern struct thd_autoinc_service_st {
  void (*thd_get_autoinc_func)(const MYSQL_THD thd,
                               unsigned long* off, unsigned long* inc);
} *thd_autoinc_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
#define thd_get_autoinc(thd, off, inc) \
  (thd_autoinc_service->thd_get_autoinc_func((thd), (off), (inc)))
#else
/**
  Return autoincrement system variables
  @param  IN  thd   user thread connection handle
  @param  OUT off   the value of @@SESSION.auto_increment_offset
  @param  OUT inc   the value of @@SESSION.auto_increment_increment
*/
void thd_get_autoinc(const MYSQL_THD thd,
                     unsigned long* off, unsigned long* inc);
#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_THD_AUTOINC_INCLUDED
#endif
