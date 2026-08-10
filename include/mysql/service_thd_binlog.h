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

int thd_is_current_stmt_binlog_format_row(const MYSQL_THD thd);

int thd_rpl_use_binlog_events_for_fk_cascade(const MYSQL_THD thd);

/*
  Report an FK-cascade row change performed by a storage engine on a child
  table. The storage engine only supplies the child TABLE and the affected
  row image(s) in MySQL record format; the server decides whether and how to
  binlog the change. It queues the reported rows in execution order, marks the
  resulting row events (FK_CASCADE_EVENTS_F etc.), and flushes them into the
  binary log at statement end / commit (discarding them on rollback). The
  server copies the supplied record buffers, so the caller may reuse or free
  them once the call returns.
*/
void thd_binlog_cascade_delete_row(MYSQL_THD thd, struct TABLE *table,
                                   const unsigned char *before_record);

void thd_binlog_cascade_update_row(MYSQL_THD thd, struct TABLE *table,
                                   const unsigned char *before_record,
                                   const unsigned char *after_record);

#ifdef __cplusplus
}
#endif
