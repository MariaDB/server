--source include/have_innodb_binlog.inc
# Test does a lot of queries that take a lot of CPU under Valgrind.
--source include/not_valgrind.inc

--source include/reset_master.inc

let $basedir=$MYSQLTEST_VARDIR/tmp/backup;

CREATE TABLE t1(a varchar(60) PRIMARY KEY, b VARCHAR(60)) ENGINE INNODB;
INSERT INTO t1 VALUES(1, NULL);
CREATE TABLE t2 (val INT) ENGINE=InnoDB;
INSERT INTO t2 VALUES (0);

--disable_query_log
--delimiter //
CREATE PROCEDURE gen_load()
  MODIFIES SQL DATA
  BEGIN
    DECLARE i INT;
    DECLARE flag TYPE OF t2.val;
    SET i = 0;
    load_loop: LOOP
      SELECT val INTO flag FROM t2;
      IF NOT (flag=0) THEN
        LEAVE load_loop;
      END IF;
      START TRANSACTION;
      INSERT INTO t1 VALUES (CONCAT("AbAdCaFe", LPAD(i, 6, "0")), @@SESSION.last_gtid);
      COMMIT;
      SET i = i + 1;
    END LOOP;
  END
//
--delimiter ;
--enable_query_log

connect (con1,localhost,root,,);
--echo *** Start a background load...
send CALL gen_load();

--connection default
--echo *** Doing backup...
--exec $XTRABACKUP --defaults-file=$MYSQLTEST_VARDIR/my.cnf --backup --target-dir=$basedir $backup_args
--echo *** Doing prepare...
--exec $XTRABACKUP --defaults-file=$MYSQLTEST_VARDIR/my.cnf --prepare --target-dir=$basedir

--echo *** Stop the background load...
UPDATE t2 SET val=1;
--connection con1
reap;
--connection default
disconnect con1;
--let $count_master= `SELECT COUNT(*) FROM t1`

--echo *** Provision a new slave from the backup
--connect (server2,127.0.0.1,root,,,$SERVER_MYPORT_2)
--let $datadir_2= `SELECT @@datadir`

--echo *** Stopping provisioned server
--source include/shutdown_mysqld.inc

--echo *** Removing old datadir for provisioned server
--rmdir $datadir_2

--echo *** Provision new server from backup
--exec $XTRABACKUP --copy-back --datadir=$datadir_2 --target-dir=$basedir $copy_back_args

--let $restart_parameters= --skip-slave-start
--source include/start_mysqld.inc

--let $gtid_pos= `SELECT @@GLOBAL.gtid_binlog_pos`

--replace_result $gtid_pos GTID_POS
eval SET GLOBAL gtid_slave_pos= '$gtid_pos';
--replace_result $SERVER_MYPORT_1 SERVER_MYPORT_1
eval CHANGE MASTER TO
  master_ssl_verify_server_cert=0,
  master_port=$SERVER_MYPORT_1, master_host='127.0.0.1', master_user='root',
  master_use_gtid= Slave_pos;
START SLAVE;

--connection default
--source include/save_master_gtid.inc

--connection server2
--source include/sync_with_master_gtid.inc
--let $count_slave= `SELECT COUNT(*) FROM t1`
if ($count_master != $count_slave) {
  --echo *** ERROR: Table on master has $count_master rows, but table on provisioned slave has $count_slave rows
  --die Row difference on provisioned slave.
}

# Cleanup

--connection server2
STOP SLAVE;
RESET SLAVE ALL;
DROP PROCEDURE gen_load;
DROP TABLE t1, t2;

--connection default
DROP PROCEDURE gen_load;
DROP TABLE t1, t2;

rmdir $basedir;
