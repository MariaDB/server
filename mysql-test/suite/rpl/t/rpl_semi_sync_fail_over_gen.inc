# Initial slave
--connection server_2
--source include/stop_slave.inc

# Initial master
--connection server_1
RESET MASTER;
SET @@global.max_binlog_size= 4096;

--connection server_2
RESET MASTER;
SET @@global.max_binlog_size= 4096;
set @@global.rpl_semi_sync_slave_enabled = 1;
set @@global.gtid_slave_pos = "";
CHANGE MASTER TO master_use_gtid= slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
set @@global.rpl_semi_sync_master_enabled = 1;
set @@global.rpl_semi_sync_master_wait_point=AFTER_SYNC;

CREATE TABLE t1 (a INT PRIMARY KEY, b MEDIUMTEXT) ENGINE=Innodb;
INSERT INTO t1 VALUES (1, 'dummy1');
--save_master_pos

# the actual slave server will have stopped the slave service at exit
--source $scenario

--echo #
--echo # Cleanup
--echo #
# current slave
--connection server_1
set global rpl_semi_sync_slave_enabled = 0;
set global rpl_semi_sync_master_enabled = 0;
set global rpl_semi_sync_master_wait_point=default;
RESET MASTER;
RESET SLAVE;

--connection server_2
RESET MASTER;
RESET SLAVE;
set @@global.rpl_semi_sync_master_enabled = 0;
set @@global.rpl_semi_sync_slave_enabled = 0;
set @@global.rpl_semi_sync_master_wait_point=default;
evalp CHANGE MASTER TO master_host='127.0.0.1', master_port=$SERVER_MYPORT_1, master_user='root', master_use_gtid=SLAVE_POS;
set @@global.gtid_slave_pos=@@global.gtid_binlog_pos;
--source include/start_slave.inc

--connection server_1
DROP TABLE t1;
--save_master_pos

--connection server_2
--sync_with_master

connection default;
--enable_reconnect
--source include/wait_until_connected_again.inc

