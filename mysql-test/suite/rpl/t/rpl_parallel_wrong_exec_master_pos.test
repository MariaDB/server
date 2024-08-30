--echo *** MDEV-5938: Exec_master_log_pos not updated at log rotate in parallel replication ***
--source include/have_innodb.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET GLOBAL slave_parallel_threads=1;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t5 (a INT PRIMARY KEY, b INT);
INSERT INTO t5 VALUES (1,1);
INSERT INTO t5 VALUES (2,2), (3,8);
INSERT INTO t5 VALUES (4,16);
--save_master_pos

--connection server_2
--sync_with_master
let $io_file= query_get_value(SHOW SLAVE STATUS, Master_Log_File, 1);
let $io_pos= query_get_value(SHOW SLAVE STATUS, Read_Master_Log_Pos, 1);
let $sql_file= query_get_value(SHOW SLAVE STATUS, Relay_Master_Log_File, 1);
let $sql_pos= query_get_value(SHOW SLAVE STATUS, Exec_Master_Log_Pos, 1);
--disable_query_log
eval SELECT IF('$io_file' = '$sql_file', "OK", "Not ok, $io_file <> $sql_file") AS test_check;
eval SELECT IF('$io_pos' = '$sql_pos', "OK", "Not ok, $io_pos <> $sql_pos") AS test_check;
--enable_query_log

--connection server_1
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc
--save_master_pos

--connection server_2
--sync_with_master
let $io_file= query_get_value(SHOW SLAVE STATUS, Master_Log_File, 1);
let $io_pos= query_get_value(SHOW SLAVE STATUS, Read_Master_Log_Pos, 1);
let $sql_file= query_get_value(SHOW SLAVE STATUS, Relay_Master_Log_File, 1);
let $sql_pos= query_get_value(SHOW SLAVE STATUS, Exec_Master_Log_Pos, 1);
--disable_query_log
eval SELECT IF('$io_file' = '$sql_file', "OK", "Not ok, $io_file <> $sql_file") AS test_check;
eval SELECT IF('$io_pos' = '$sql_pos', "OK", "Not ok, $io_pos <> $sql_pos") AS test_check;
--enable_query_log

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t5;

--source include/rpl_end.inc
