--source include/galera_cluster.inc
--source include/force_restart.inc

# Save original auto_increment_offset values.
--let $node_1=node_1
--let $node_2=node_2
--source include/auto_increment_offset_save.inc

--connection node_1
reset master;
--connection node_2
reset master;

#
# Test Galera with --log-bin --log-slave-updates .
# This way the actual MySQL binary log is used,
# rather than Galera's own implementation
#

CREATE TABLE t1 (id INT PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);

CREATE TABLE t2 (id INT) ENGINE=InnoDB;
INSERT INTO t2 VALUES (1);
INSERT INTO t2 VALUES (1);

--connection node_2
SELECT COUNT(*) = 1 FROM t1;
SELECT COUNT(*) = 2 FROM t2;

--connection node_1
ALTER TABLE t1 ADD COLUMN f2 INTEGER;
--let $MASTER_MYPORT=$NODE_MYPORT_1
--source include/show_binlog_events.inc

--connection node_2

#--connection node_2
#--source suite/galera/include/galera_stop_replication.inc

--echo Shutting down server ...
--source include/shutdown_mysqld.inc

--connection node_1
--let $wait_condition = SELECT VARIABLE_VALUE = 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc

#
# Force SST
#
--echo Cleaning var directory ...
--remove_file $MYSQLTEST_VARDIR/mysqld.2/data/grastate.dat
--remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.2/data/mtr
--remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.2/data/performance_schema
--remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.2/data/test
--remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.2/data/mysql
--remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.2/data

--connection node_2

--echo Starting server ...
let $restart_noprint=2;
--source include/start_mysqld.inc

--let $wait_condition = SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
--source include/wait_condition.inc

--let $wait_condition = SELECT VARIABLE_VALUE = 'ON' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_ready';
--source include/wait_condition.inc

--connection node_2
SELECT COUNT(*) = 2 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_NAME = 't1';
--let $MASTER_MYPORT=$NODE_MYPORT_2
--source include/show_binlog_events.inc

DROP TABLE t1;
DROP TABLE t2;

--echo #cleanup
--connection node_1
RESET MASTER;

# Restore original auto_increment_offset values.
--source include/auto_increment_offset_restore.inc
