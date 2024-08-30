--echo *** MDEV_6435: Incorrect error handling when query binlogged partially on master with "killed" error ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET GLOBAL slave_parallel_threads=1;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t6 (a INT) ENGINE=MyISAM;
CREATE TRIGGER tr AFTER INSERT ON t6 FOR EACH ROW SET @a = 1;
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)

--connection con1
--let $conid = `SELECT CONNECTION_ID()`
SET debug_sync='sp_head_execute_before_loop SIGNAL ready WAIT_FOR cont';
send INSERT INTO t6 VALUES (1), (2), (3);

--connection server_1
SET debug_sync='now WAIT_FOR ready';
--replace_result $conid CONID
eval KILL QUERY $conid;
SET debug_sync='now SIGNAL cont';

--connection con1
--error ER_QUERY_INTERRUPTED
--reap
SET debug_sync='RESET';
--let $after_error_gtid_pos= `SELECT @@gtid_binlog_pos`

--connection server_1
SET debug_sync='RESET';

--connection server_2
--let $slave_sql_errno= 1317
--source include/wait_for_slave_sql_error.inc
STOP SLAVE IO_THREAD;
--replace_result $after_error_gtid_pos AFTER_ERROR_GTID_POS
eval SET GLOBAL gtid_slave_pos= '$after_error_gtid_pos';
--source include/start_slave.inc

--connection server_1
INSERT INTO t6 VALUES (4);
SELECT * FROM t6 ORDER BY a;
--save_master_pos

--connection server_2
--sync_with_master
SELECT * FROM t6 ORDER BY a;

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP TABLE t6;
SET DEBUG_SYNC= 'RESET';
--disconnect con1

--source include/rpl_end.inc
