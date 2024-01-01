--let $primary=server_1
--let $replica=server_2

--connection $primary
delimiter |;
CREATE PROCEDURE sp_xa(val int)
BEGIN
 XA START   'xid';
 INSERT INTO t1 VALUES (val, REPEAT("x", 4100));
 XA END     'xid';
 XA PREPARE 'xid';
END|
delimiter ;|

--connection $replica
--sync_with_master

# $case corresponds to execution env created by rpl_semi_sync_crash.inc
# invoked with $query_to_crash etc.
--let $case = 1

--echo #
--echo # Case:$case.A
--echo #
--echo # CRASH $primary/master $primary after XA PREPARE semisynced
--echo # FAILOVER to $replica/slave

--connection $primary
--let $next_val=`SELECT max(a)+1 FROM t1`
--let $query_to_crash= CALL sp_xa($next_val)
--echo # $query_to_crash
--let $truncate_gtid=0-1-5
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 1
--let $expected_rows_on_slave= 1
--let $failover_to_slave=1
--source rpl_semi_sync_crash.inc

--echo #
--echo # $primary promoted to the primary/master will serve new replica/slave $replica starting from $truncate_gtid
--echo #

--connection $primary
--let $rows_so_far=2
--eval XA COMMIT 'xid'
--save_master_pos
--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc

--echo #
--echo # Case:$case.B
--echo #
--echo # CRASH the primary/master after a sequence of
--echo # XA PREPARE, XA COMMIT (XAC becomes durable in Engine), Trx is binlogged
--echo # but remains only prepared by crash time.
--echo # FAILOVER to $replica.

--connection $primary
--let $next_val=`SELECT max(a)+1 FROM t1`
--eval CALL sp_xa($next_val)
--save_master_pos

--connection $replica
--sync_with_master

--connection $primary
XA COMMIT 'xid';
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc

--let $next_val=`SELECT max(a)+1 FROM t1`
--let $query_to_crash= INSERT INTO t1 VALUES ($next_val, REPEAT("x", 4100))
--echo # $query_to_crash
--let $truncate_gtid=0-2-9
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 3
--let $expected_rows_on_slave= 4
--let $failover_to_slave=0
--source rpl_semi_sync_crash.inc

--echo #
--echo # $primary promoted to the primary/master will serve new replica/slave $replica starting from $truncate_gtid
--echo #

--connection $primary
--eval DELETE FROM t1 WHERE a = $next_val
--let $rows_so_far=3
--save_master_pos
--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc


--echo #
--echo # Case:$case.C
--echo #
--echo # CRASH the primary/master after a sequence of
--echo # XA PREPARE, XA ROLLBACK. XAR won't be durable in Engine and will be
--echo # discarded from truncated binlog.
--echo # XAP remains prepared after crash-recovery on the former
--echo # demoted-to-slave master.
--echo # FAILOVER to $replica.

--connection $primary
call mtr.add_suppression("Found 1 prepared XA transactions");

--let $next_val=`SELECT max(a)+1 FROM t1`
--let $pre_query_to_crash=CALL sp_xa($next_val)
--let $query_to_crash= XA ROLLBACK 'xid'
--echo # $query_to_crash
--let $truncate_gtid=0-1-13
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 3
--let $expected_rows_on_slave= 3
--let $failover_to_slave=1
--source rpl_semi_sync_crash.inc

let $pre_query_to_crash=;

--echo #
--echo # $primary promoted to the primary/master will serve new replica/slave $replica starting from $truncate_gtid
--echo #

--connection $primary
--let $next_val=`SELECT max(a)+1 FROM t1`
--eval INSERT INTO t1 VALUES ($next_val, REPEAT("x", 4100));
--let $rows_so_far=4
--save_master_pos
--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc


--let $case=3
--echo #
--echo # Case:$case.A
--echo #
--echo # CRASH the primary/master after a sequence of
--echo # XA PREPARE, Trx, Rotate.
--echo # Both XAP and Trx are discarded from truncated binlog.
--echo # FAILOVER to $replica.

--connection $primary
--let $next_val=`SELECT max(a)+1 FROM t1`
--let $query_to_crash= CALL sp_xa($next_val)
--let $query2_to_crash=INSERT INTO t1 VALUES ($next_val+1, REPEAT("x", 4100))
--echo # $query_to_crash
--let $truncate_gtid=0-2-15
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 4
--let $expected_rows_on_slave= 4
--let $failover_to_slave=0
--source rpl_semi_sync_crash.inc

--connection $replica
--echo # no xid in the list
XA RECOVER;

--connection $primary
--eval XA COMMIT 'xid'
--let $rows_so_far=5
--save_master_pos
--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc


--let $case=3
--echo #
--echo # Case:$case.B
--echo #
--echo # CRASH the primary/master after a sequence of
--echo # XA PREPARE, Rotate, Trx, Rotate, XA COMMIT.
--echo # Trx and XA-COMMIT are discarded from truncated binlog.
--echo # FAILOVER to $replica.
# XAP recovers because it's "made" durable by the binlogged XAC.
# Trx ($query_to_crash) is to truncate from.

--connection $primary
--let $next_val=`SELECT max(a)+1 FROM t1`
# $pre_query_to_crash and $query2_to_crash run by the same connection
--let $pre_query_to_crash=CALL sp_xa($next_val)
--let $query_to_crash=INSERT INTO t1 VALUES ($next_val+1, REPEAT("x", 4100))
--let $query2_to_crash=XA COMMIT 'xid'
--echo # $pre_query_to_crash
--echo # $query_to_crash
--echo # $query2_to_crash
--let $truncate_gtid=0-1-18
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 5
--let $expected_rows_on_slave= 6
--let $failover_to_slave=1
--source rpl_semi_sync_crash.inc

--echo #
--echo # $primary promoted to the primary/master will serve new replica/slave $replica starting from $truncate_gtid
--echo #

--connection $primary
# complete the xa
--eval $query2_to_crash
--save_master_pos

--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc

let $pre_query_to_crash=;
let $pre_query_to_crash_2=;

--let $case=3
--echo #
--echo # Case:$case.C
--echo #
--echo # CRASH the primary/master after a sequence of
--echo #   XAP_20(xid), Rotate in the last stable log followed by
--echo #   XAC_21(xid), XAP_23(xid_2), XAC_24(xid_2), XAP_25(xid), \
--echo #      Rotate(1->2), BCP(1), Trx_26, Rotate(2->3), XAR_27(xid).
--echo # FAILOVER to $replica.
# Simulate no Binlog Check-Point BCP(1) in the rotated (2,3) logs which
# causes the entire XAC_21 etc sequence involvement into recovery.
# Truncation from Trx_26, because XAR_27 clears doubts about XAP_25 to
# keep as prepared.

--connection $primary

--let $next_val=`SELECT max(a)+1 FROM t1`
--eval CALL sp_xa($next_val)
XA COMMIT 'xid';
--inc $next_val

delimiter |;
CREATE PROCEDURE sp_xa2(val int)
BEGIN
 XA START   'xid_2';
 INSERT INTO t1 VALUES (val, REPEAT("x", 1));
 XA END     'xid_2';
 XA PREPARE 'xid_2';
END|
delimiter ;|

--eval CALL sp_xa2($next_val)
XA COMMIT 'xid_2';
--inc $next_val

# $pre_query_to_crash and $query2_to_crash run by the same connection
--let $pre_query_to_crash  =CALL sp_xa($next_val)
--let $query_to_crash= INSERT INTO t1 VALUES ($next_val+1, REPEAT("4", 4100))
--let $query2_to_crash= XA ROLLBACK 'xid'
--echo # XAP_1
--echo # XAC_2
--echo # $pre_query_to_crash
--echo # $query_to_crash
--echo # $query2_to_crash
--let $truncate_gtid=0-2-26
--echo # Expected State post crash:
--let $log_search_pattern=truncated binlog file.*to remove transactions starting from GTID $truncate_gtid
--let $expected_rows_on_master= 9
--let $expected_rows_on_slave= 10
--let $failover_to_slave=`select ($failover_to_slave+1) % 2`
--let $dbug_restart="--debug-dbug=+d,simulate_stale_binlog_checkpoint"
#--let $debug_pre_crash=select sleep(60)
--source rpl_semi_sync_crash.inc

--echo #
--echo # $primary promoted to the primary/master will serve new replica/slave $replica starting from $truncate_gtid
--echo #

--connection $primary
# complete the xa
--eval $query2_to_crash
--save_master_pos

--echo # The gtid state on current master must be equal to ...
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';
SHOW VARIABLES LIKE 'gtid_slave_pos';

--connection $replica
--sync_with_master
--eval SELECT COUNT(*) = $rows_so_far as 'true' FROM t1
--echo # ... the gtid states on the slave:
SHOW VARIABLES LIKE 'gtid_slave_pos';
SHOW VARIABLES LIKE 'gtid_binlog_pos';
SHOW VARIABLES LIKE 'gtid_binlog_state';

--let $diff_tables=server_1:t1, server_2:t1
--source include/diff_tables.inc

let $pre_query_to_crash  =;
let $pre_query_to_crash_2=;

--echo # end of the cases

--connection $primary
drop procedure sp_xa;
drop procedure sp_xa2;

--connection $replica
--sync_with_master
--source include/stop_slave.inc
