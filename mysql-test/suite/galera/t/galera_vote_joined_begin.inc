# This file purpose is to set up node 4 to require SST which is artificaially
# prolonged and as a result accumulate sufficient relication queue.
# The contents of the qeuee are controlled in the sourcing test files.

--source include/galera_cluster.inc
--source include/big_test.inc
--source include/have_debug_sync.inc

--let $node_1=node_1
--let $node_2=node_2
--let $node_3=node_3
--let $node_4=node_4
--source ../include/auto_increment_offset_save.inc

# create table t1 and procedure p1 to generate wirtesets
--connection node_1
CREATE TABLE t1(pk INT AUTO_INCREMENT PRIMARY KEY);

DELIMITER |;
CREATE PROCEDURE p1(IN max INT)
BEGIN
  DECLARE i INT;
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION BEGIN END;

  SET i = 0;
  WHILE i < max DO
    INSERT IGNORE INTO t1 VALUES (DEFAULT);
    SET i = i + 1;
  END WHILE;
END|
DELIMITER ;|

# 130 events move the commit cut, it is essential in voting
CALL p1(130);

--connection node_4
--echo Shutting down server 4...
--let $node_4_server_id= `SELECT @@server_id`
--let $node_4_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.$node_4_server_id.expect
--let $node_4_pid_file= `SELECT @@pid_file`
--source include/shutdown_mysqld.inc
# enforce SST
--exec rm -rf $MYSQLTEST_VARDIR/mysqld.4/data/grastate.dat

# Wait for node #4 to leave cluster
--connection node_1
--let $members = 3
--source include/wsrep_wait_membership.inc

# prepare to stop SST donor thread when node is in donor state
SET GLOBAL debug = "+d,sync.wsrep_donor_state";

--connection node_4
--echo Restarting server 4...
# Need to use this form instead of start_mysqld.inc because the latter is blocking
--exec echo "restart:$start_mysqld_params" > $node_4_expect_file_name

# Wait for node #1 to become a donor
--connection node_1
SET SESSION DEBUG_SYNC = "now WAIT_FOR sync.wsrep_donor_state_reached";
--echo Tables on server 1 flushed and locked for SST to server 4
SET SESSION DEBUG_SYNC = "now SIGNAL signal.wsrep_donor_state";
SET GLOBAL debug = "";
SET DEBUG_SYNC='RESET';

--echo Wait for the state snapshot to be copied to server 4
--source include/galera_wait_ready.inc
--echo SST script unlocked server 1

#
# At this point state snapshot has been copied, node 1 is operational and
# we have about 20 seconds while everything we do will go into the replication
# queue on node 4 which it will have to apply on top of the snapshot.
#
