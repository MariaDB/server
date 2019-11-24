--source include/master-slave.inc
--source include/have_binlog_format_row.inc
--source include/not_embedded.inc
--source include/not_valgrind.inc

call mtr.add_suppression("Slave SQL: Could not execute Write_rows event on table test.t1");
call mtr.add_suppression(".*Worker.*failed executing transaction");
call mtr.add_suppression(".*The slave coordinator and worker threads are stopped");

--disable_warnings
drop table if exists t1;
--enable_warnings

connection master;
CREATE TABLE t1 (id int primary key, value int) engine=RocksDB;
sync_slave_with_master;
--let $rsbm = query_get_value(select @@global.reset_seconds_behind_master, @@global.reset_seconds_behind_master, 1)
set global reset_seconds_behind_master=1;

connection slave;
INSERT INTO t1 VALUES(1, 0);
INSERT INTO t1 VALUES(2, 0);
INSERT INTO t1 VALUES(3, 0);

connection master;
sync_slave_with_master;
connection master;
INSERT INTO t1 VALUES(1, 1);

connection slave;
--let $slave_sql_errno= 1062
--let $not_switch_connection= 0
--let $slave_timeout= 120
--source include/wait_for_slave_sql_error_and_skip.inc
set global reset_seconds_behind_master=0;
--source include/stop_slave_io.inc

connection master;
INSERT INTO t1 values (4,0);
--sleep 11
INSERT INTO t1 VALUES(2, 1);

connection slave;
--source include/start_slave_io.inc

connection master;
sync_slave_with_master;

connection slave;
set global reset_seconds_behind_master=1;

connection master;
insert into t1 values (5,0);
--sleep 1
sync_slave_with_master;

connection master;
INSERT INTO t1 VALUES(3, 1);

connection slave;
--let $slave_sql_errno= 1062
--let $not_switch_connection= 0
--source include/wait_for_slave_sql_error_and_skip.inc

--echo #
--echo # Cleanup
--echo #

connection master;
DROP TABLE t1;
eval set global reset_seconds_behind_master=$rsbm;
--source include/rpl_end.inc
