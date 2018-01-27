/*
   Copyright (c) 2012,2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

namespace myrocks {

/*
  Declare INFORMATION_SCHEMA (I_S) plugins needed by MyRocks storage engine.
*/

extern struct st_mysql_plugin rdb_i_s_cfstats;
extern struct st_mysql_plugin rdb_i_s_dbstats;
extern struct st_mysql_plugin rdb_i_s_perf_context;
extern struct st_mysql_plugin rdb_i_s_perf_context_global;
extern struct st_mysql_plugin rdb_i_s_cfoptions;
extern struct st_mysql_plugin rdb_i_s_compact_stats;
extern struct st_mysql_plugin rdb_i_s_global_info;
extern struct st_mysql_plugin rdb_i_s_ddl;
extern struct st_mysql_plugin rdb_i_s_index_file_map;
extern struct st_mysql_plugin rdb_i_s_lock_info;
extern struct st_mysql_plugin rdb_i_s_trx_info;
extern struct st_mysql_plugin rdb_i_s_deadlock_info;
} // namespace myrocks
