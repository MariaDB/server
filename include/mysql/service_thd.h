/* Copyright (c) 2026, MariaDB Corporation.

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

#ifndef MYSQL_SERVICE_THD_INCLUDED
#define MYSQL_SERVICE_THD_INCLUDED
  
/**
  @file include/mysql/service_thd.h
  This service provides functions for plugins and storage engines to access
  current thd.
*/

/**
  @defgroup plugin_api_service_thd THD service
  @ingroup plugin_api_services
  Access to the current thd

  This service provides functions for plugins and storage engines to access
  current thd.

  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

extern struct thd_service_st {
  MYSQL_THD (*get_current_thd)(void);
} *thd_service;

/**
  Current session (thd) accessor
  @return pointer to current thd
*/
#ifdef MYSQL_DYNAMIC_PLUGIN
# define get_current_thd() thd_service->get_current_thd()
#else
MYSQL_THD get_current_thd();
#endif

#ifdef __cplusplus
}
#endif

/** @} */

#endif
