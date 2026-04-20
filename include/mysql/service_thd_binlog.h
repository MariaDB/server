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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct TABLE;
class Event_log;
class binlog_cache_data;

int thd_is_current_stmt_binlog_format_row(const MYSQL_THD thd);

int thd_rpl_use_binlog_events_for_fk_cascade(const MYSQL_THD thd);

void thd_binlog_mark_fk_cascade_events(MYSQL_THD thd);

int thd_binlog_update_row(MYSQL_THD thd, struct TABLE *table,
                          class Event_log *bin_log,
                          class binlog_cache_data *cache_data,
                          int is_trans, unsigned long row_image,
                          const unsigned char *before_record,
                          const unsigned char *after_record);

int thd_binlog_delete_row(MYSQL_THD thd, struct TABLE *table,
                          class Event_log *bin_log,
                          class binlog_cache_data *cache_data,
                          int is_trans, unsigned long row_image,
                          const unsigned char *before_record);

#ifdef __cplusplus
}
#endif
