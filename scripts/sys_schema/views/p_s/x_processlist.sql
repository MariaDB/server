-- Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

--
-- View: x$processlist
--
-- A detailed non-blocking processlist view to replace 
-- [INFORMATION_SCHEMA. | SHOW FULL] PROCESSLIST
-- 
-- mysql> select * from x$processlist where conn_id is not null\G
-- *************************** 1. row ***************************
--                 thd_id: 23
--                conn_id: 4
--                   user: msandbox@localhost
--                     db: test
--                command: Query
--                  state: altering table
--                   time: 19
--      current_statement: alter table t1 add column j int
--      statement_latency: 19466744276374
--           lock_latency: 539307000000
--          rows_examined: 0
--              rows_sent: 0
--          rows_affected: 0
--             tmp_tables: 0
--        tmp_disk_tables: 0
--              full_scan: NO
--         last_statement: NULL
-- last_statement_latency: NULL
--              last_wait: wait/io/file/innodb/innodb_temp_file
--      last_wait_latency: 63400681890
--                 source: row0merge.cc:780
--

CREATE OR REPLACE
  ALGORITHM = TEMPTABLE
  DEFINER = 'mariadb.sys'@'localhost'
  SQL SECURITY INVOKER
VIEW x$processlist (
  thd_id,
  conn_id,
  user,
  db,
  command,
  state,
  time,
  current_statement,
  statement_latency,
  lock_latency,
  rows_examined,
  rows_sent,
  rows_affected,
  tmp_tables,
  tmp_disk_tables,
  full_scan,
  last_statement,
  last_statement_latency,
  last_wait,
  last_wait_latency,
  source
) AS
SELECT pps.thread_id AS thd_id,
       pps.processlist_id AS conn_id,
       IF(pps.name = 'thread/sql/one_connection',
          CONCAT(pps.processlist_user, '@', pps.processlist_host),
          REPLACE(pps.name, 'thread/', '')) user,
       pps.processlist_db AS db,
       pps.processlist_command AS command,
       pps.processlist_state AS state,
       pps.processlist_time AS time,
       pps.processlist_info AS current_statement,
       IF(esc.end_event_id IS NULL,
          esc.timer_wait,
          NULL) AS statement_latency,
       esc.lock_time AS lock_latency,
       esc.rows_examined AS rows_examined,
       esc.rows_sent AS rows_sent,
       esc.rows_affected AS rows_affected,
       esc.created_tmp_tables AS tmp_tables,
       esc.created_tmp_disk_tables AS tmp_disk_tables,
       IF(esc.no_good_index_used > 0 OR esc.no_index_used > 0, 'YES', 'NO') AS full_scan,
       IF(esc.end_event_id IS NOT NULL,
          esc.sql_text,
          NULL) AS last_statement,
       IF(esc.end_event_id IS NOT NULL,
          esc.timer_wait,
          NULL) AS last_statement_latency,
       ewc.event_name AS last_wait,
       IF(ewc.end_event_id IS NULL AND ewc.event_name IS NOT NULL,
          'Still Waiting', 
          ewc.timer_wait) last_wait_latency,
       ewc.source
  FROM performance_schema.threads AS pps
  LEFT JOIN performance_schema.events_waits_current AS ewc USING (thread_id)
  LEFT JOIN performance_schema.events_statements_current as esc USING (thread_id)
 ORDER BY pps.processlist_time DESC, last_wait_latency DESC;
