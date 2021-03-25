/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

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

#ifndef SQL_REPL_INCLUDED
#define SQL_REPL_INCLUDED

#include "rpl_filter.h"

#ifdef HAVE_REPLICATION
#include "slave.h"

struct slave_connection_state;

extern my_bool opt_show_slave_auth_info;
extern char *master_host, *master_info_file;

extern int max_binlog_dump_events;
extern my_bool opt_sporadic_binlog_dump_fail;

int start_slave(THD* thd, Master_info* mi, bool net_report);
int stop_slave(THD* thd, Master_info* mi, bool net_report);
bool change_master(THD* thd, Master_info* mi, bool *master_info_added);
bool mysql_show_binlog_events(THD* thd);
int reset_slave(THD *thd, Master_info* mi);
int reset_master(THD* thd, rpl_gtid *init_state, uint32 init_state_len,
                 ulong next_log_number);
bool purge_master_logs(THD* thd, const char* to_log);
bool purge_master_logs_before_date(THD* thd, time_t purge_time);
bool log_in_use(const char* log_name);
void adjust_linfo_offsets(my_off_t purge_offset);
void show_binlogs_get_fields(THD *thd, List<Item> *field_list);
bool show_binlogs(THD* thd);
extern int init_master_info(Master_info* mi);
void kill_zombie_dump_threads(uint32 slave_server_id);
int check_binlog_magic(IO_CACHE* log, const char** errmsg);
int compare_log_name(const char *log_1, const char *log_2);

struct LOAD_FILE_IO_CACHE : public IO_CACHE
{
  THD* thd;
  my_off_t last_pos_in_file;
  bool wrote_create_file, log_delayed;
  int (*real_read_function)(struct st_io_cache *,uchar *,size_t);
};

int log_loaded_block(IO_CACHE* file, uchar *Buffer, size_t Count);
int init_replication_sys_vars();
void mysql_binlog_send(THD* thd, char* log_ident, my_off_t pos, ushort flags);

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_LOCK_slave_state, key_LOCK_binlog_state;
#endif
void rpl_init_gtid_slave_state();
void rpl_deinit_gtid_slave_state();
void rpl_init_gtid_waiting();
void rpl_deinit_gtid_waiting();
int gtid_state_from_binlog_pos(const char *name, uint32 pos, String *out_str);
int rpl_append_gtid_state(String *dest, bool use_binlog);
int rpl_load_gtid_state(slave_connection_state *state, bool use_binlog);
bool rpl_gtid_pos_check(THD *thd, char *str, size_t len);
bool rpl_gtid_pos_update(THD *thd, char *str, size_t len);
#else

struct LOAD_FILE_IO_CACHE : public IO_CACHE { };

#endif /* HAVE_REPLICATION */

#endif /* SQL_REPL_INCLUDED */
