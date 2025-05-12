/* Copyright (c) 2013, 2018, MariaDB

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

#ifndef MYSQL_SERVICE_LOG_WARNINGS
#define MYSQL_SERVICE_LOG_WARNINGS

/**
  @file
  This service provides access to the log warning level for the
  current session.

  thd_log_warnings(thd)
  @return thd->log_warnings
*/

#ifdef __cplusplus
extern "C" {
#endif

extern struct thd_log_warnings_service_st {
  void *(*thd_log_warnings)(MYSQL_THD);
} *thd_log_warnings_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
# define thd_log_warnings(THD) thd_log_warnings_service->thd_log_warnings(THD)
#else
/**
  MDL_context accessor
  @param thd   the current session
  @return pointer to thd->mdl_context
*/
int thd_log_warnings(MYSQL_THD thd);
#endif

#ifdef __cplusplus
}
#endif

#endif

