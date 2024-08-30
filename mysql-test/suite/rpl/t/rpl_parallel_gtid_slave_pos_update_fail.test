# MDEV-6549, failing to update gtid_slave_pos for a transaction that was retried.

# The problem was that when a transaction updates the mysql.gtid_slave_pos
# table, it clears the flag that marks that there is a GTID position that
# needs to be updated. Then, if the transaction got killed after that due
# to a deadlock, the subsequent retry would fail to notice that the GTID needs
# to be recorded in gtid_slave_pos.
#
# (In the original bug report, the symptom was an assertion; this was however
# just a side effect of the missing update of gtid_slave_pos, which also
# happened to cause a missing clear of OPTION_GTID_BEGIN).
--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
# Must use statement-based binlogging. Otherwise the transaction will not be
# binlogged at all, as it modifies no rows.
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t4 (a INT PRIMARY KEY, b INT, KEY b_idx(b)) ENGINE=InnoDB;
INSERT INTO t4 VALUES (1,NULL), (2,2), (3,NULL), (4,4), (5, NULL), (6, 6);
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con2,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc

# Create two transactions that can run in parallel on the slave but cause
# a deadlock if the second runs before the first.
--connection con1
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send UPDATE t4 SET b=NULL WHERE a=6;
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 1;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
--connection con2
REAP;
SET debug_sync='RESET';
--save_master_pos
--let $last_gtid= `SELECT @@last_gtid`

--connection server_2
# Disable the usual skip of gap locks for transactions that are run in
# parallel, using DBUG. This allows the deadlock to occur, and this in turn
# triggers a retry of the second transaction, and the code that was buggy and
# caused the gtid_slave_pos update to be skipped in the retry.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,disable_thd_need_ordering_with";
--source include/start_slave.inc
--sync_with_master
SET GLOBAL debug_dbug=@old_dbug;

SELECT * FROM t4 ORDER BY a;
# Check that the GTID of the second transaction was correctly recorded in
# gtid_slave_pos, in the variable as well as in the table.
--replace_result $last_gtid GTID
eval SET @last_gtid= '$last_gtid';
SELECT IF(@@gtid_slave_pos LIKE CONCAT('%',@last_gtid,'%'), "GTID found ok",
    CONCAT("GTID ", @last_gtid, " not found in gtid_slave_pos=", @@gtid_slave_pos))
    AS result;
SELECT "ROW FOUND" AS `Is the row found?`
  FROM mysql.gtid_slave_pos
 WHERE CONCAT(domain_id, "-", server_id, "-", seq_no) = @last_gtid;

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP TABLE t4;
SET DEBUG_SYNC= 'RESET';
--disconnect con1
--disconnect con2
--source include/rpl_end.inc
