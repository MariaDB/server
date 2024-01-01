# $query1          a query that creates a group of events that goes into binlog
# $query2          a query that follows $query1 in binlog without having done
#                    engine commit
# $truncate_index  binlog file index that undergoes truncation
# $truncate_gtid   the first gtid in the cut off tail part of
#                    the binlog index' file

connect(master1,localhost,root,,);
--echo "List of binary logs before rotation"
--source include/show_binary_logs.inc

# Some load to either non- and transactional egines
# that should not affect the following recovery:
INSERT INTO ti VALUES(1,"I am gonna survive");

# XAP_1 will be holding a mutex ..
SET DEBUG_SYNC= "commit_after_release_LOCK_after_binlog_sync SIGNAL master1_ready WAIT_FOR master1_go_never_arrives";
--send_eval $query1

connect(master2,localhost,root,,);
# .. to not let the 2nd trx to commit.
SET DEBUG_SYNC= "now WAIT_FOR master1_ready";
SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL master2_ready WAIT_FOR master2_go_never_arrives";
--send_eval $query2

--connection default
SET DEBUG_SYNC= "now WAIT_FOR master2_ready";

connect(master3,localhost,root,,);
# The 3nd trx will be considered at recovery, but as it won't get into binlog
# it won't recover.
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL master3_ready WAIT_FOR master3_go_never_arrives";
--send INSERT INTO ti VALUES (4, "not gonna be logged therefore survive"),(5, "ditto")

--connection default
SET DEBUG_SYNC= "now WAIT_FOR master3_ready";

--echo "List of binary logs before crash"
--source include/show_binary_logs.inc
--echo # The gtid binlog state prior the crash will be truncated at the end of the test
SELECT @@global.gtid_binlog_state;

--connection default
--source include/kill_mysqld.inc
--disconnect master1
--disconnect master2
--disconnect master3

#
# Server restart
#
--let $restart_parameters= --rpl-semi-sync-slave-enabled=1 --sync-binlog=1 --log-warnings=3
--source include/start_mysqld.inc

# Check error log for a successful truncate message.
let $log_error_ = $MYSQLTEST_VARDIR/log/mysqld.1.err;

--let SEARCH_FILE=$log_error_
--let SEARCH_PATTERN=Successfully truncated.*to remove transactions starting from GTID $truncate_gtid
--source include/search_pattern_in_file.inc
--let SEARCH_PATTERN=truncated binlog file:.*master.*00000$truncate_index
--source include/search_pattern_in_file.inc

--echo # One record should be present in table
SELECT * FROM ti;
--echo # The truncated gtid binlog state
SELECT @@global.gtid_binlog_state;
SELECT @@global.gtid_binlog_pos;
DELETE FROM ti;

