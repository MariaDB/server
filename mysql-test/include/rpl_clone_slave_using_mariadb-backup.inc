if ($cnf == "galera2_to_mariadb")
{
  --let MASTER_MYPORT= $NODE_MYPORT_1
  --connect master, 127.0.0.1, root, , test, $NODE_MYPORT_1
  --connect slave, 127.0.0.1, root, , test, $NODE_MYPORT_3
  --disable_query_log
  --replace_result $MASTER_MYPORT ###
  --eval CHANGE MASTER TO MASTER_HOST='127.0.0.1', MASTER_USER='root', MASTER_PORT=$MASTER_MYPORT, MASTER_USE_GTID=NO, MASTER_SSL_VERIFY_SERVER_CERT=0;
  --enable_query_log
  START SLAVE;
  --source include/wait_for_slave_to_start.inc

  --let XTRABACKUP_BACKUP_OPTIONS=--no-defaults --user=root --host='127.0.0.1' --port=$NODE_MYPORT_3 --loose-disable-ssl-verify-server-cert --loose-ssl-cert=$MYSQL_TEST_DIR/std_data/server-cert.pem --loose-ssl-key=$MYSQL_TEST_DIR/std_data/server-key.pem --loose-ssl-ca=$MYSQL_TEST_DIR/std_data/cacert.pem

  --let XTRABACKUP_COPY_BACK_OPTIONS=--no-defaults
}

if ($cnf == "mariadb_to_mariadb")
{
  --let XTRABACKUP_BACKUP_OPTIONS=--defaults-file=$MYSQLTEST_VARDIR/my.cnf --defaults-group-suffix=.2
  --let XTRABACKUP_COPY_BACK_OPTIONS=--defaults-file=$MYSQLTEST_VARDIR/my.cnf --defaults-group-suffix=.2
}

--connection master
--let $MYSQLD_DATADIR_MASTER= `select @@datadir`
--connection slave
--let $MYSQLD_DATADIR_SLAVE= `select @@datadir`

# This test covers the filename:pos based synchronization
# between the master and the slave.
# If we ever need to test a GTID based synchronization,
# it should be done in a separate test.


--echo ##############################################################
--echo ### Initial block with some transactions

--echo ### Slave: Make sure replication is not using GTID
--connection slave
--let $value= query_get_value(SHOW SLAVE STATUS, "Using_Gtid", 1)
--echo # Using_Gtid=$value

--echo ### Master: Create and populate t1
--connection master
CREATE TABLE t1(a TEXT) ENGINE=InnoDB CHARSET=latin1;
START TRANSACTION;
INSERT INTO t1 VALUES ('tr#00:stmt#00 - slave run#0, before backup');
INSERT INTO t1 VALUES ('tr#00:stmt#01 - slave run#0, before backup');
INSERT INTO t1 VALUES ('tr#00:stmt#02 - slave run#0, before backup');
COMMIT;
--sync_slave_with_master



--echo ##############################################################
--echo ### Run the last transaction before mariadb-backup --backup
--echo ### Remember SHOW MASTER STATUS and @@gtid_binlog_pos
--echo ### before and after the transaction.

--echo ### Master: Rember MASTER STATUS and @@gtid_binlog_pos before tr#01
--connection master
--let $master_before_tr01_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $master_before_tr01_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $master_before_tr01_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`

--echo ### Slave: Remember MASTER STATUS and @@gtid_binlog_pos before tr#01
--connection slave
--let $slave_before_tr01_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $slave_before_tr01_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $slave_before_tr01_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`

--echo ### Master: Run the actual last transaction before the backup
--connection master
START TRANSACTION;
INSERT INTO t1 VALUES ('tr#01:stmt#00 - slave run#0, before backup');
INSERT INTO t1 VALUES ('tr#01:stmt#01 - slave run#0, before backup');
INSERT INTO t1 VALUES ('tr#01:stmt#02 - slave run#0, before backup');
COMMIT;
--sync_slave_with_master

--echo ### Master: Remember MASTER STATUS and @@gtid_binlog_pos after tr#01
--connection master
--let $master_after_tr01_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $master_after_tr01_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $master_after_tr01_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`

--echo ### Slave: Remember MASTER STATUS and @@gtid_binlog_pos after tr#01
--connection slave
--let $slave_after_tr01_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $slave_after_tr01_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $slave_after_tr01_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`


--echo ##############################################################
--echo ### Running `mariadb-backup --backup,--prepare` and checking
--echo ### that mariadb_backup_slave_info and mariadb_backup_binlog_info are OK

--echo ### Slave: Create a backup
--let $backup_slave=$MYSQLTEST_VARDIR/tmp/backup-slave
--disable_result_log
--exec $XTRABACKUP $XTRABACKUP_BACKUP_OPTIONS --slave-info --backup --target-dir=$backup_slave
--enable_result_log

--echo ### Slave: Prepare the backup
--exec $XTRABACKUP --prepare --target-dir=$backup_slave

--echo ### Slave: xtrabackup files:
--echo ############################ mariadb_backup_slave_info
--replace_result $master_after_tr01_show_master_status_file master_after_tr01_show_master_status_file $master_after_tr01_show_master_status_position master_after_tr01_show_master_status_position
--cat_file $backup_slave/mariadb_backup_slave_info
--echo ############################ mariadb_backup_binlog_info
--replace_result $slave_after_tr01_show_master_status_file slave_after_tr01_show_master_status_file $slave_after_tr01_show_master_status_position slave_after_tr01_show_master_status_position $slave_after_tr01_gtid_binlog_pos slave_after_tr01_gtid_binlog_pos
--cat_file $backup_slave/mariadb_backup_binlog_info
--echo ############################


--echo ##############################################################
--echo ### Run more transactions after the backup:
--echo ### - while the slave is still running, then
--echo ### - while the slave is shut down

--echo ### Master: Run another transaction while the slave is still running
--connection master
START TRANSACTION;
INSERT INTO t1 VALUES ('tr#02:stmt#00 - slave run#0, after backup');
INSERT INTO t1 VALUES ('tr#02:stmt#01 - slave run#0, after backup');
INSERT INTO t1 VALUES ('tr#02:stmt@02 - slave run#0, after backup');
COMMIT;
--sync_slave_with_master

--echo ### Master: Remember MASTER STATUS and @@gtid_binlog_pos after tr#02
--connection master
--let $master_after_tr02_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $master_after_tr02_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $master_after_tr02_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`

--echo ### Slave: Remember MASTER STATUS and @@gtid_binlog_pos after tr#02
--connection slave
--let $slave_after_tr02_show_master_status_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $slave_after_tr02_show_master_status_position=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $slave_after_tr02_gtid_binlog_pos=`SELECT @@global.gtid_binlog_pos`


--echo ### Master: Checking SHOW BINLOG EVENTS

--connection master
--vertical_results
### The BEGIN event
--replace_column 4 # 5 #
--replace_result $master_after_tr01_show_master_status_file master_after_tr01_show_master_status_file $master_after_tr01_show_master_status_position master_after_tr01_show_master_status_position $master_after_tr02_gtid_binlog_pos master_after_tr02_gtid_binlog_pos
--eval SHOW BINLOG EVENTS IN '$master_after_tr01_show_master_status_file' FROM $master_after_tr01_show_master_status_position LIMIT 0,1
### The INSERT event
--replace_column 2 # 4 # 5 #
--replace_result $master_after_tr01_show_master_status_file master_after_tr01_show_master_status_file $master_after_tr01_show_master_status_position master_after_tr01_show_master_status_position
# Hide the difference between row and stmt binary logging
--replace_regex /use `test`; // /(Query|Annotate_rows)/Query_or_Annotate_rows/
--eval SHOW BINLOG EVENTS IN '$master_after_tr01_show_master_status_file' FROM $master_after_tr01_show_master_status_position LIMIT 1,1
--horizontal_results

--echo ### Slave: Checking SHOW BINLOG EVENTS
--connection slave
--vertical_results
### The BEGIN event
--replace_column 2 # 5 #
--replace_result $slave_after_tr01_show_master_status_file slave_after_tr01_show_master_status_file $slave_after_tr01_show_master_status_position slave_after_tr01_show_master_status_position $slave_after_tr02_gtid_binlog_pos slave_after_tr02_gtid_binlog_pos
--eval SHOW BINLOG EVENTS IN '$slave_after_tr01_show_master_status_file' FROM $slave_after_tr01_show_master_status_position LIMIT 0,1
### The INSERT event
--replace_column 2 # 4 # 5 #
--replace_result $slave_after_tr01_show_master_status_file slave_after_tr01_show_master_status_file $slave_after_tr01_show_master_status_position slave_after_tr01_show_master_status_position $slave_after_tr02_gtid_binlog_pos slave_after_tr02_gtid_binlog_pos
# Hide the difference between row and stmt binary logging
--replace_regex /use `test`; // /(Query|Annotate_rows)/Query_or_Annotate_rows/
--eval SHOW BINLOG EVENTS IN '$slave_after_tr01_show_master_status_file' FROM $slave_after_tr01_show_master_status_position LIMIT 1,1
--horizontal_results

--echo ### Slave: Stop replication
--connection slave
STOP SLAVE;
--source include/wait_for_slave_to_stop.inc
RESET SLAVE;

--echo ### Slave: Shutdown the server

if ($cnf == "mariadb_to_mariadb")
{
  --let $rpl_server_number= 2
  --source include/rpl_stop_server.inc
}

if ($cnf == "galera2_to_mariadb")
{
  --connection slave
  --source $MYSQL_TEST_DIR/include/shutdown_mysqld.inc
}

--echo ### Master: Run a transaction while the slave is shut down
--connection master
START TRANSACTION;
INSERT INTO t1 VALUES ('tr#03:stmt#00 - after slave run#0, slave is shut down, after backup');
INSERT INTO t1 VALUES ('tr#03:stmt#01 - after slave run#0, slave is shut down, after backup');
INSERT INTO t1 VALUES ('tr#03:stmt#02 - after slave run#0, slave is shut down, after backup');
COMMIT;


--echo ##############################################################
--echo ### Emulate starting a new virgin slave

--echo ### Slave: Remove the data directory
--rmdir $MYSQLD_DATADIR_SLAVE

--echo ### Slave: Copy back the backup
--exec $XTRABACKUP $XTRABACKUP_COPY_BACK_OPTIONS --copy-back --datadir=$MYSQLD_DATADIR_SLAVE --target-dir=$backup_slave

--echo ### Slave: Restart the server
if ($cnf == "mariadb_to_mariadb")
{
  --let $rpl_server_number= 2
  --source include/rpl_start_server.inc
  --source include/wait_until_connected_again.inc
}

if ($cnf == "galera2_to_mariadb")
{
  --connection slave
  --source $MYSQL_TEST_DIR/include/start_mysqld.inc
}

--echo ### Slave: Display the restored data before START SLAVE
--connection slave
SELECT * FROM t1 ORDER BY a;

--echo ### Slave: Execute the CHANGE MASTER statement to set up the host and port
--replace_result $MASTER_MYPORT ###
--eval CHANGE MASTER '' TO MASTER_USER='root', MASTER_HOST='127.0.0.1', MASTER_PORT=$MASTER_MYPORT, MASTER_CONNECT_RETRY=1, MASTER_SSL_VERIFY_SERVER_CERT=0

--echo ### Slave: Execute the CHANGE MASTER statement from mariadb_backup_slave_info
--replace_result $master_after_tr01_show_master_status_file master_after_tr01_show_master_status_file $master_after_tr01_show_master_status_position master_after_tr01_show_master_status_position
--source $backup_slave/mariadb_backup_slave_info

--echo ### Slave: Execute START SLAVE
--source include/start_slave.inc

--echo ### Master: Wait for the slave to apply all master events
--connection master
--sync_slave_with_master slave

--echo ### Slave: Make sure replication is not using GTID after the slave restart
--connection slave
--let $value= query_get_value(SHOW SLAVE STATUS, "Using_Gtid", 1)
--echo # Using_Gtid=$value

--echo ### Slave: Display the restored data after START SLAVE
--connection slave
SELECT * FROM t1 ORDER BY a;


--echo ##############################################################
--echo ### Continue master transactions, check the new slave replicates well.

--echo ### Master: Run a transaction after restarting replication
--connection master
START TRANSACTION;
INSERT INTO t1 VALUES ('tr#04:stmt#00 - slave run#1');
INSERT INTO t1 VALUES ('tr#04:stmt#01 - slave run#1');
INSERT INTO t1 VALUES ('tr#04:stmt#02 - slave run#1');
COMMIT;
--sync_slave_with_master

--echo ### Slave: Display the restored data + new transactions
--connection slave
SELECT * FROM t1 ORDER BY a;


--echo ##############################################################
--echo ### Cleanup

--echo ### Removing the backup directory
--rmdir $backup_slave

--connection master
DROP TABLE t1;
--sync_slave_with_master

if ($cnf == "mariadb_to_mariadb")
{
  --source include/rpl_end.inc
}

if ($cnf == "galera2_to_mariadb")
{
  STOP SLAVE;
  --source include/wait_for_slave_to_stop.inc
  RESET SLAVE ALL;

  --connection master
  set global wsrep_on=OFF;
  RESET MASTER;
  set global wsrep_on=ON;
}
