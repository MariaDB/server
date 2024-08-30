#ifndef MARIADB_PLUGIN_DATA_TYPE_INCLUDED
#define MARIADB_PLUGIN_DATA_TYPE_INCLUDED
/* Copyright (C) 2019, Alexander Barkov and MariaDB

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

  Data Type Plugin API.

  This file defines the API for server plugins that manage data types.
*/

#ifdef __cplusplus

#include <mysql/plugin.h>

/*
  API for data type plugins. (MariaDB_DATA_TYPE_PLUGIN)
*/
#define MariaDB_DATA_TYPE_INTERFACE_VERSION (MYSQL_VERSION_ID << 8)


struct st_mariadb_data_type
{
  int interface_version;
  class Type_handler *type_handler;
};


/**
  Data type plugin descriptor
*/

#endif /* __cplusplus */

#endif /* MARIADB_PLUGIN_DATA_TYPE_INCLUDED */
