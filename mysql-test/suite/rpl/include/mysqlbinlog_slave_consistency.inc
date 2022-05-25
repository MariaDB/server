# This file provides the structure to run a single test that ensures the
# mariadb-binlog command line tool is consistent with replicas for event
# filtering. The test is configured using the following input parameters, where
# each is nullable (i.e. it will not be used to configure mariadb-binlog or
# the replica).
#
# param $do_domain_ids     : A list of domain ids to include in replication
# param $ignore_domain_ids : A list of domain ids to exclude from replication
# param $ignore_server_ids : A list of server ids to exclude from replication
# param $start_position    : The GTID positions to begin replication from in
#                            the specified domains
# param $stop_position     : The GTID positions that mark the end of an event
#                            stream in a particular domain
#
# param $con1              : The connection name of the primary server
# param $con2              : The connection name of the replica server
# param $strict_mode       : Uses input and checks for out of order GTIDs
# param $strict_mode_err   : A boolean that provides expectations for strict
#                            mode to error
# param $slave_sql_errno   : Expected error number of the slave SQL thread


--let $include_filename= mysqlbinlog_slave_consistency.inc
--source include/begin_include_file.inc

--enable_query_log

if (!$con1)
{
    --let $con1=master
}
if (!$con2)
{
    --let $con2=slave
}

if (!$strict_mode)
{
    --connection $con2
    set @@global.gtid_strict_mode=0;
    --let $sql_input_file=include/sql_multisource.inc
}

if ($strict_mode)
{
    --connection $con2
    set @@global.gtid_strict_mode=1;
    --let $sql_input_file=include/sql_out_of_order_gtid.inc
}

--connection $con2
--source include/stop_slave.inc

--connection $con1
--echo # Populating $con1 data
--source $sql_input_file
--source include/save_master_gtid.inc

--let $MYSQLD_DATADIR=`select @@datadir`
--let $MYSQLBINLOG_STDERR=$MYSQLD_DATADIR/mysqlbinlog_stderr.out
--let BINLOG_FILENAME= query_get_value(SHOW BINARY LOGS, Log_name, 1)
--let BINLOG_FILE_PARAM= $MYSQLD_DATADIR/$BINLOG_FILENAME.orig
--copy_file $MYSQLD_DATADIR/$BINLOG_FILENAME $BINLOG_FILE_PARAM

--connection $con2
--let $msbl_args=
if (`SELECT strcmp("$start_position","") != 0`)
{
    eval set global gtid_slave_pos="$start_position";
    --let $msbl_args= $msbl_args --start-position=$start_position
}

--let $cm_args= MASTER_USE_GTID=slave_pos
if (`SELECT strcmp("$do_domain_ids","") != 0`)
{
    --let $cm_args= $cm_args, DO_DOMAIN_IDS=($do_domain_ids)
    --let $msbl_args= $msbl_args --do-domain-ids=$do_domain_ids
}
if (`SELECT strcmp("$ignore_domain_ids","") != 0`)
{
    --let $cm_args= $cm_args, IGNORE_DOMAIN_IDS=($ignore_domain_ids)
    --let $msbl_args= $msbl_args --ignore-domain-ids=$ignore_domain_ids
}
if (`SELECT strcmp("$ignore_server_ids","") != 0`)
{
    --let $cm_args= $cm_args, IGNORE_SERVER_IDS=($ignore_server_ids)
    --let $msbl_args= $msbl_args --ignore-server-ids=$ignore_server_ids
}
if ($strict_mode)
{
    --let $msbl_args= $msbl_args --gtid-strict-mode
}
eval CHANGE MASTER TO $cm_args;

--let $start_slave_args=
if (`SELECT strcmp("$stop_position","") != 0`)
{
    --let $start_slave_args= UNTIL master_gtid_pos="$stop_position"
    --let $msbl_args= $msbl_args --stop-position=$stop_position
}

eval START SLAVE $start_slave_args;

if ($slave_sql_errno)
{
    --echo # $con2 SQL Thread error expected - waiting for errno $slave_sql_errno
    --source include/wait_for_slave_sql_error.inc
}

# If we are not expecting an error, wait for con2 to catch up
if (!$slave_sql_errno)
{
    --echo # No $con2 error expecting - waiting for $con2 to catch up to $con1

    # Stop position was not specified
    if (`SELECT strcmp("$stop_position","") = 0`)
    {
        --source include/sync_with_master_gtid.inc
    }

    # Stop position was specified
    if (`SELECT strcmp("$stop_position","") != 0`)
    {
        --echo # Because there is a stop position we wait for all events to process
        --echo # and $con2 to automatically stop
        --source include/wait_for_slave_to_stop.inc
    }
}

--echo # Stop $con2 so it stops receiving $con1 events.
--source include/stop_slave.inc

--connection $con1
DROP TABLE IF EXISTS t1, t2, t3, t4, t5;
RESET MASTER;
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM $msbl_args 2> MYSQLBINLOG_STDERR | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM $msbl_args 2> $MYSQLBINLOG_STDERR | $MYSQL

--source include/rpl_check_table_consistency.inc

if ($strict_mode)
{
    --echo # Strict mode enabled - checking mysqlbinlog error output for out
    --echo #                       of order GTIDs
    --let SEARCH_FILE=$MYSQLBINLOG_STDERR
    --let SEARCH_PATTERN=Found out of order GTID
    if ($strict_mode_err)
    {
        --echo # Expecting to find out of order GTID error..
    }
    if (!$strict_mode_err)
    {
        --echo # Not expecting to find out of order GTID error..
    }
    --source include/search_pattern_in_file.inc
}

--echo # Test finished - resetting $con1 and $con2..
--connection $con2
RESET SLAVE;
RESET MASTER;
set global gtid_slave_pos="";
CHANGE MASTER TO DO_DOMAIN_IDS=(), IGNORE_DOMAIN_IDS=(), IGNORE_SERVER_IDS=();

--connection $con1
RESET MASTER;
DROP TABLE IF EXISTS t1, t2, t3, t4, t5;
--source include/save_master_gtid.inc

--connection $con2
--source include/start_slave.inc
--source include/wait_for_slave_to_start.inc
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
RESET SLAVE;
set global gtid_slave_pos="";
RESET MASTER;

--connection $con1
RESET MASTER;

--connection $con2
if ($strict_mode)
{
    set @@global.gtid_strict_mode=0;
}
--source include/start_slave.inc

--connection $con1
--remove_file $BINLOG_FILE_PARAM
--remove_file $MYSQLBINLOG_STDERR

--let $include_filename= mysqlbinlog_slave_consistency.inc
--source include/end_include_file.inc
