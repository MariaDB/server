/* Copyright (c) 2015, 2017, Tencent.
Use is subject to license terms

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef REPL_SPEEDLIMIT_PLUGIN_VARS_H
#define REPL_SPEEDLIMIT_PLUGIN_VARS_H

#include "repl_speedlimit_util.h"
#include "repl_speed_monitor.h"
#include "sql_class.h"                          // THD

extern RplSpeedMonitor  speed_monitor;

#ifdef HAVE_PSI_INTERFACE

void init_psi_keys(void);

#endif /* HAVE_PSI_INTERFACE */

#define SHOW_FNAME(name)      \
  rpl_speed_limit_show_##name

#define DEF_SHOW_FUNC(name, show_type)                    \
  static int SHOW_FNAME(name)(MYSQL_THD thd, SHOW_VAR *var, char *buff)  \
{                                     \
  speed_monitor.setExportStatus();                    \
  var->type = show_type;                          \
  var->value = (char *)&rpl_speed_limit_##name;           \
  return 0;                               \
 }

/*
  speedlimit system variables
 */
extern SYS_VAR* repl_speed_limit_system_vars[];

/* plugin status variables */
extern SHOW_VAR repl_speed_limit_status_vars[];

#endif /* REPL_SPEEDLIMIT_PLUGIN_VARS_H */
