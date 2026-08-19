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
  @file include/mysql/service_thd_mdl.h
  This service provides functions for plugins and storage engines to access
  metadata locks.
*/

/**
  @defgroup plugin_api_service_thd_mdl THD MDL service
  @ingroup plugin_api_services
  Access to the metadata locks.

  This service provides functions for plugins and storage engines to access
  metadata locks.
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif


extern struct thd_mdl_service_st {
  void *(*thd_mdl_context)(MYSQL_THD);
} *thd_mdl_service;

/**
  MDL_context accessor
  @param thd   the current session
  @return pointer to thd->mdl_context
*/
#ifdef MYSQL_DYNAMIC_PLUGIN
# define thd_mdl_context(thd) thd_mdl_service->thd_mdl_context(thd)
#else
void *thd_mdl_context(MYSQL_THD thd);
#endif

#ifdef __cplusplus
}
#endif

/** @} */
