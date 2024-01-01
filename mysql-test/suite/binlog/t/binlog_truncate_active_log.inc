#
# Meanings of parameters:
#
# $query1       a query that creates a group of events that goes into binlog
# $query2       a query that follows $query1 in binlog without having done
#                 engine commit
# $query20      an optinal $query2 that durably commits in engine
# $delete       when 1 deletes from t
# $no_truncate  when 1 NOT FOUND off search_pattern_in_file.inc is "good".
#
connect(master1,localhost,root,,);
connect(master2,localhost,root,,);
connect(master3,localhost,root,,);
connect(master4,localhost,root,,);

--connection default

# First to commit few transactions
INSERT INTO t  VALUES (10);
INSERT INTO tm VALUES (10);

--connection master1
if (!$query20)
{
  # Hold insert after write to binlog and before "run_commit_ordered" in engine
  SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL master1_ready WAIT_FOR signal_never_arrives";
  --send_eval $query1
}
if ($query20)
{
  # do not hold the $query2 insert to let it binlog past $query1 "us"
  --eval $query1
}

--connection master2
if (!$query20)
{
  SET DEBUG_SYNC= "now WAIT_FOR master1_ready";
  SET DEBUG_SYNC= "commit_before_get_LOCK_after_binlog_sync SIGNAL master2_ready";  --send_eval $query2
}
if ($query20)
{
  SET DEBUG_SYNC= "commit_before_get_LOCK_after_binlog_sync SIGNAL master2_ready";
  --eval $query20
}

--connection master3
SET DEBUG_SYNC= "now WAIT_FOR master2_ready";
SELECT @@global.gtid_binlog_pos as 'Before the crash';

--connection master4
# Simulate prepared & not-logged trx; it will never recover.
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL master4_ready WAIT_FOR signal_never_arrives";
--send INSERT INTO t4  VALUES (13)

--connection master3
SET DEBUG_SYNC= "now WAIT_FOR master4_ready";
SELECT @@global.gtid_binlog_pos as 'Before the crash and never logged trx';

--connection default
--source include/kill_mysqld.inc
--disconnect master1
--disconnect master2
--disconnect master3
--disconnect master4

#
# Server restart
#
--let $restart_parameters= --rpl-semi-sync-slave-enabled=1 --sync-binlog=1 --log-warnings=3
--source include/start_mysqld.inc

# Check error log for a successful truncate message.
if ($no_truncate)
{
  --echo # *** Expect NOT FOUND /Successfully truncated ... which is correct. ***
}
--let $log_error_ = $MYSQLTEST_VARDIR/log/mysqld.1.err

--let SEARCH_FILE=$log_error_
--let SEARCH_PATTERN=Successfully truncated.*to remove transactions starting from GTID $truncate_gtid_pos

--source include/search_pattern_in_file.inc

--echo Pre-crash binlog file content:
--let $binlog_file= query_get_value(show binary logs, Log_name, $binlog_file_index)
--source include/show_binlog_events.inc

SELECT @@global.gtid_binlog_pos as 'After the crash';
--echo "One row should be present in table 't'"
SELECT * FROM t;
--echo "No row should be present in table 't4'"
SELECT * FROM t4;

# prepare binlog file index for the next test
--inc $binlog_file_index

# Local cleanup
if ($delete)
{
  DELETE FROM t;
}
