/* Copyright (c) 2017, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
@file mysql/plugin_clone.h
API for clone plugin
*/

#ifndef MYSQL_PLUGIN_CLONE_INCLUDED
#define MYSQL_PLUGIN_CLONE_INCLUDED

#include "my_global.h"
#include "violite.h"
#include "plugin.h"

/** Clone plugin interface version */
#define MariaDB_CLONE_INTERFACE_VERSION 0x0100

/**
  The descriptor structure for the plugin, that is referred from
  st_mysql_plugin.
*/

struct Mysql_clone {
  /** clone plugin interface version */
  int interface_version;

  /** Clone database from local server.
  @param[in]	thd		server thread handle
  @param[in]	data_dir	cloned data directory
  @return error code, 0 on success */
  int (*clone_local)(THD *thd, const char *data_dir);
};

/** Create clone handle to  access the clone interfaces from server.
Called when Clone plugin is installed.
@param[in]	plugin_name	clone plugin name
@return error code */
int clone_handle_create(const char *plugin_name);

/** Drop clone handle. Called when Clone plugin is uninstalled.
@return error code */
int clone_handle_drop();

/** Check if it is safe to uninstall clone plugin.
@param[in,out]	plugin_info	plugin
@return error code */
int clone_handle_check_drop(MYSQL_PLUGIN plugin_info);

#endif
