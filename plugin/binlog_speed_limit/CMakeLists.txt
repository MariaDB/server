# Copyright (c) 2015, 2017, Tencent and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA


SET(REPLICATION_SPEEDLIMIT_MASTER_SOURCES
 repl_speedlimit_master_plugin.cc
 repl_speedlimit_plugin_vars.cc
 repl_speedlimit_util.cc
 repl_speed_monitor.cc
 repl_speedlimit_plugin_vars.h
 repl_speedlimit_util.h
 repl_speed_monitor.h)

MYSQL_ADD_PLUGIN(repl_speedlimit_master ${REPLICATION_SPEEDLIMIT_MASTER_SOURCES}
  RECOMPILE_FOR_EMBEDDED)

#MYSQL_ADD_PLUGIN(repl_speedlimit_master ${REPLICATION_SPEEDLIMIT_MASTER_SOURCES}
#  MODULE_ONLY MODULE_OUTPUT_NAME "repl_speedlimit_master")

SET(REPLICATION_SPEEDLIMIT_SLAVE_SOURCES
 repl_speedlimit_slave_plugin.cc
 repl_speedlimit_plugin_vars.cc
 repl_speedlimit_util.cc
 repl_speed_monitor.cc
 repl_speedlimit_plugin_vars.h
 repl_speedlimit_util.h
 repl_speed_monitor.h)

MYSQL_ADD_PLUGIN(repl_speedlimit_slave ${REPLICATION_SPEEDLIMIT_SLAVE_SOURCES}
  RECOMPILE_FOR_EMBEDDED)

#MYSQL_ADD_PLUGIN(repl_speedlimit_slave ${REPLICATION_SPEEDLIMIT_SLAVE_SOURCES}
#  MODULE_ONLY MODULE_OUTPUT_NAME "repl_speedlimit_slave")
