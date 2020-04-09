#
# Loop body of binlog_truncate_multi_engine.test
# Parameters:
#   $debug_sync_action  describes debug-sync actions
#   $kill_server        1 when to crash, 0 for regular restart
#   $restart_parameters the caller may simulate partial commit at recovery
#   $test_outcome       summary of extected results
#   $MYSQLD_DATADIR

--echo #
--echo #
--echo # Case $case
--echo #
RESET MASTER;
FLUSH LOGS;
SET GLOBAL max_binlog_size= 4096;

connect(con1,localhost,root,,);
#--connection con1
--echo List of binary logs before rotation
--source include/show_binary_logs.inc
INSERT INTO t1 VALUES (1, REPEAT("x", 1));
INSERT INTO t2 VALUES (1, REPEAT("x", 1));
BEGIN;
  INSERT INTO t1 VALUES (2, REPEAT("x", 4100));
  INSERT INTO t2 VALUES (2, REPEAT("x", 4100));

--eval SET DEBUG_SYNC= $debug_sync_action
send COMMIT;

--connection default
SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
--echo List of binary logs after rotation
--source include/show_binary_logs.inc

--echo # restart the server with $restart_parameters
if ($kill_server)
{
  --echo # the server is crashed
  --source include/kill_mysqld.inc
  --source include/start_mysqld.inc
}
if (!$kill_server)
{
  --echo # the server is restarted
  --source include/restart_mysqld.inc
}

--connection default
--echo #
--echo # *** Summary: $test_outcome:
--echo #
SELECT COUNT(*) FROM t1;
SELECT COUNT(*) FROM t2;
SELECT @@GLOBAL.gtid_binlog_state;
SELECT @@GLOBAL.gtid_binlog_pos;
--echo List of binary logs at the end of the tests
--source include/show_binary_logs.inc
--echo # ***
# cleanup
DELETE FROM t1;
DELETE FROM t2;
--disconnect con1
--echo #
