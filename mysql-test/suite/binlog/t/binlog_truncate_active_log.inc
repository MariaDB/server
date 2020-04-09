connect(master1,localhost,root,,);
connect(master2,localhost,root,,);
connect(master3,localhost,root,,);

--connection default

# First to commit few transactions
INSERT INTO t  VALUES (10);
INSERT INTO tm VALUES (10);

--connection master1
# Hold insert after write to binlog and before "run_commit_ordered" in engine
SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL master1_ready WAIT_FOR signal_never_arrives";
--send_eval $query1

--connection master2
SET DEBUG_SYNC= "now WAIT_FOR master1_ready";
if ($pre_q2)
{
  CALL sp_blank_xa;
}
SET DEBUG_SYNC= "commit_before_get_LOCK_after_binlog_sync SIGNAL master2_ready";
# To binlog non-xid transactional group which will be truncated all right
--send_eval $query2


--connection master3
SET DEBUG_SYNC= "now WAIT_FOR master2_ready";
SELECT @@global.gtid_binlog_pos as 'Before the crash';

--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF

--source include/kill_mysqld.inc
--source include/wait_until_disconnected.inc
#
# Server restart
#
--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart: --rpl-semi-sync-slave-enabled=1
EOF

connection default;
--enable_reconnect
--source include/wait_until_connected_again.inc

# Check error log for a successful truncate message.
let $log_error_= `SELECT @@GLOBAL.log_error`;
if(!$log_error_)
{
    # MySQL Server on windows is started with --console and thus
    # does not know the location of its .err log, use default location
    let $log_error_ = $MYSQLTEST_VARDIR/log/mysqld.1.err;
}
--let SEARCH_FILE=$log_error_
--let SEARCH_RANGE=-50000
--let SEARCH_PATTERN=$this_search_pattern
--replace_regex /FOUND [0-9]+/FOUND #/
--source include/search_pattern_in_file.inc

SELECT @@global.gtid_binlog_pos as 'After the crash';
--echo "One row should be present in table 't'"
SELECT COUNT(*) as 'One' FROM t;

# Local cleanup
DELETE FROM t;
--disconnect master1
--disconnect master2
--disconnect master3
