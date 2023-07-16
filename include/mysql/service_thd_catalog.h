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
  @file include/mysql/service_thd_catalog.h
  This service provides access to the current catalog
*/

#ifdef __cplusplus
extern "C" {
#endif

class SQL_CATALOG;

extern struct thd_catalog_service_st {
  SQL_CATALOG *(*thd_catalog_context)(MYSQL_THD);
  const char *(*thd_catalog_path)(MYSQL_THD);
} *thd_catalog_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
# define thd_catalog_context(_THD) thd_catalog_service->thd_catalog_context(_THD)
# define thd_catalog_path(_THD) thd_catalog_service->thd_catalog_path(_THD)
#else
/**
  Catalog  accessor
  @param thd   the current session
  @return pointer of thd->catalog
*/
SQL_CATALOG *thd_catalog_context(MYSQL_THD thd);
const char *thd_catalog_path(MYSQL_THD thd);
#endif

#ifdef __cplusplus
}
#endif
