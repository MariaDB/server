#
# This file tests that a master can be demoted to a slave using
# CHANGE MASTER TO MASTER_DEMOTE_TO_SLAVE=1 and replication will work correctly
# afterwards.
#
# param $ignore_domain_ids : List of GTID domain ids to use for
#                            CHANGE MASTER TO IGNORE_DOMAIN_IDS
#

--let $include_filename= rpl_change_master_demote.inc
--source include/begin_include_file.inc

--echo ##############################################
--echo # Connection semantics change:
--echo #  * True primary is now connection 'slave'
--echo #  * True replica is now connection 'master'
--echo ##############################################

--connection master
SELECT VARIABLE_NAME, GLOBAL_VALUE FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE VARIABLE_NAME LIKE 'gtid_binlog_pos' OR VARIABLE_NAME LIKE 'gtid_slave_pos' OR VARIABLE_NAME LIKE 'gtid_current_pos' ORDER BY VARIABLE_NAME ASC;

--let $extra_cm_args=
if (`SELECT strcmp("$ignore_domain_ids","") != 0`)
{
    --let $extra_cm_args=, ignore_domain_ids=($ignore_domain_ids)
}

--echo # First ensure gtid_slave_pos after master_demote_to_slave matches
--echo # gtid_current_pos calculation
--let $current_pos= `SELECT @@gtid_current_pos`
--replace_result $SLAVE_MYPORT SLAVE_PORT
--eval CHANGE MASTER TO master_host='127.0.0.1', master_port=$SLAVE_MYPORT, master_user='root', master_use_gtid=slave_pos, master_demote_to_slave=1 $extra_cm_args
SELECT VARIABLE_NAME, GLOBAL_VALUE FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE VARIABLE_NAME LIKE 'gtid_binlog_pos' OR VARIABLE_NAME LIKE 'gtid_slave_pos' OR VARIABLE_NAME LIKE 'gtid_current_pos' ORDER BY VARIABLE_NAME ASC;
--let $slave_pos= `SELECT @@gtid_slave_pos`
--echo # Validating gtid_slave_pos == gtid_binlog_pos..
if ($slave_pos != $current_pos)
{
    SELECT VARIABLE_NAME, GLOBAL_VALUE FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE VARIABLE_NAME LIKE 'gtid_binlog_pos' OR VARIABLE_NAME LIKE 'gtid_slave_pos' OR VARIABLE_NAME LIKE 'gtid_current_pos' ORDER BY VARIABLE_NAME ASC;
    die gtid_slave_pos calculation after master_demote_to_slave=1 differs from gtid_current_pos;
}
--echo # ..success

--source include/start_slave.inc

--connection slave
--source include/stop_slave.inc

--echo # Ensuring replication works correctly after role swap
--connection slave
set session gtid_domain_id= 0;
CREATE TABLE repl_t (a int);
INSERT INTO repl_t VALUES (1);
--source include/save_master_gtid.inc

--connection master
--source include/sync_with_master_gtid.inc
--echo # Validating that replication works..
--let $n_replicated_rows= query_get_value(SELECT COUNT(*) FROM repl_t, COUNT(*), 1)
if ($n_replicated_rows != 1)
{
    die "Replication is broken";
}
--echo # ..success

--echo # Cleaning up replication check data
--connection slave
DROP TABLE repl_t;
--source include/save_master_gtid.inc

--connection master
--source include/sync_with_master_gtid.inc
SELECT VARIABLE_NAME, GLOBAL_VALUE FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE VARIABLE_NAME LIKE 'gtid_binlog_pos' OR VARIABLE_NAME LIKE 'gtid_slave_pos' OR VARIABLE_NAME LIKE 'gtid_current_pos' ORDER BY VARIABLE_NAME ASC;

--echo ##############################################
--echo # Connection semantics change:
--echo #  * True primary is back to connection 'master'
--echo #  * True replica is back to connection 'slave'
--echo ##############################################

--connection master
--source include/stop_slave.inc

--connection slave
--replace_result $MASTER_MYPORT MASTER_PORT
--eval CHANGE MASTER TO master_host='127.0.0.1', master_port=$MASTER_MYPORT, master_user='root', master_use_gtid=slave_pos, master_demote_to_slave=1
--source include/start_slave.inc

# Ensure master is ready to start sending new transactions. That is, because
# the master does not have log_slave_updates, the slave's transactions of
# repl_t are not binlogged, and its state is behind. If the master binlogs
# any new transactions before the slaves connection is matured,
# ER_GTID_POSITION_NOT_FOUND_IN_BINLOG2 can be reported to the slave
# FIXME: We shouldn't need to do this, rather, the master should use
# log-slave-updates and gtid-strict-mode
--connection master
--let $wait_condition= SELECT COUNT(*) > 0 FROM information_schema.processlist WHERE State like '%Master has sent all binlog to slave%'
--source include/wait_condition.inc

--let $include_filename= rpl_change_master_demote.inc
--source include/end_include_file.inc
