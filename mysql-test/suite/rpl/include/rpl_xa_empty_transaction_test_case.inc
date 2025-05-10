#
#   Helper script to create an XA transaction and validate it was not
# binlogged
#
# Parameters
#   $xa_completion_action     : The action to end the XA transaction, either
#                               COMMIT or ROLLBACK
#   $trx_statements           : A comma separated list specifying how to build
#                               the statements of the transaction. Each item in
#                               the list is either T (for transactional) or N
#                               (for non-transactional). An empty list will not
#                               add any statements to the transaction.
#   $use_disconnect           : When TRUE, disconnect after preparing the XA
#                               transaction to test the detach/rollback case
#

#
# Setup
--let $generic_assert_text= should not binlog XA transaction

--connection server_1
--let server_1_datadir=`select @@datadir`

--connection server_2
--let server_2_datadir=`select @@datadir`

--connection server_3
--let server_3_datadir=`select @@datadir`

--let assert_file=$MYSQLTEST_VARDIR/tmp/binlog_decoded.out

--connection server_1
CREATE TABLE tm (a INT PRIMARY KEY) engine=myisam;
CREATE TABLE ti (a INT PRIMARY KEY) engine=innodb;
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc

--connection server_1

if ($use_disconnect)
{
    --source include/count_sessions.inc
    --connect(con1,localhost,root,,)
}

XA START 'x';
--let $_stmt_items= $trx_statements
--let $_ctr= 1
while($_stmt_items)
{
  --let $_cur_stmt= `SELECT SUBSTRING_INDEX('$_stmt_items', ',', 1)`
  --let $_stmt_items= `SELECT LTRIM(SUBSTRING('$_stmt_items', LENGTH('$_cur_stmt') + 2))`

    if (`SELECT strcmp("$_cur_stmt","T") = 0`)
    {
        --let $target_table= ti
    }

    if (`SELECT strcmp("$_cur_stmt","N") = 0`)
    {
        --let $target_table= tm
    }

    --error ER_DUP_ENTRY
    --eval INSERT INTO $target_table VALUES ($_ctr),($_ctr);
    inc $_ctr;

}
XA END 'x';
XA PREPARE 'x';

if ($use_disconnect)
{
    --disconnect con1
    --connection server_1
    --source include/wait_until_count_sessions.inc
    XA RECOVER;

    --error ER_XAER_NOTA
    --eval XA $xa_completion_action 'x';
}
if (!$use_disconnect)
{
    --eval XA $xa_completion_action 'x';
}

--source include/save_master_gtid.inc

--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;

--echo # MYSQL_BINLOG server_1_datadir/binlog_filename --result-file=assert_file
--exec $MYSQL_BINLOG $server_1_datadir/$binlog_filename --result-file=$assert_file

--let assert_text= server_1 $generic_assert_text
--let assert_count= 0
--let assert_select= XA START|XA END|XA PREPARE|XA COMMIT|XA ROLLBACK
--source include/assert_grep.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;

--echo # MYSQL_BINLOG server_2_datadir/binlog_filename --result-file=assert_file
--exec $MYSQL_BINLOG $server_2_datadir/$binlog_filename --result-file=$assert_file

--let assert_text= server_2 $generic_assert_text
--source include/assert_grep.inc

--connection server_3
--source include/sync_with_master_gtid.inc
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;

--echo # MYSQL_BINLOG server_3_datadir/binlog_filename --result-file=assert_file
--exec $MYSQL_BINLOG $server_3_datadir/$binlog_filename --result-file=$assert_file

--let assert_text= server_3 $generic_assert_text
--source include/assert_grep.inc

#
# Cleanup
--connection server_1
DROP TABLE ti,tm;
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc
