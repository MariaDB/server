#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#
# Usage:
#
# --let $use_remote_mysqlbinlog= 1           # optional
# --let $binlog_start_pos= <binlog position> # optional
# --let $binlog_file= <binlog filename>      # optional
#
# --source suite/binlog/include/binlog_incident.inc
#
# The script uses MYSQLBINLOG to verify certain results.
# By default, it uses binary logs directly. If it is undesirable,
# this behavior can be overridden by setting $use_remote_binlog
# as shown above.
#
# All values will be unset after every execution of the script,
# so if they are needed, they should be set explicitly before each call.
#

# The purpose of this test is to provide a reference for how the
# incident log event is represented in the output from the mysqlbinlog
# program.

source include/have_log_bin.inc;
source include/have_debug.inc;
source include/binlog_start_pos.inc;

let $MYSQLD_DATADIR= `select @@datadir`;
RESET MASTER;

CREATE TABLE t1 (a INT);

INSERT INTO t1 VALUES (1),(2),(3);
SELECT * FROM t1;

# This will generate an incident log event and store it in the binary
# log before the replace statement.
REPLACE INTO t1 VALUES (4);

DROP TABLE t1;
FLUSH LOGS;

if ($binlog_start_pos)
{
    --let $startpos= --start-position=$binlog_start_pos
    --let $binlog_start_pos= 
}
--let $filename= master-bin.000001
if ($binlog_file)
{
    --let $filename= $binlog_file
    --let $binlog_file=
}
--let $mysqlbinlog_args= $MYSQLD_DATADIR/$filename
if ($use_remote_mysqlbinlog)
{
    --let $mysqlbinlog_args= --read-from-remote-server --protocol=tcp --host=127.0.0.1 --port=$MASTER_MYPORT -uroot $filename
    --let $use_remote_mysqlbinlog= 0
}
exec $MYSQL_BINLOG $startpos $mysqlbinlog_args >$MYSQLTEST_VARDIR/tmp/binlog_incident-bug44442.sql;
--disable_query_log
eval SELECT cont LIKE '%RELOAD DATABASE; # Shall generate syntax error%' AS `Contain RELOAD DATABASE` FROM (SELECT load_file('$MYSQLTEST_VARDIR/tmp/binlog_incident-bug44442.sql') AS cont) AS tbl;
--enable_query_log

remove_file $MYSQLTEST_VARDIR/tmp/binlog_incident-bug44442.sql;
