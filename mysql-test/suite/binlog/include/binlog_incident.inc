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
source include/have_innodb.inc;
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


--echo #
--echo # MDEV-25488 thd::transaction::modified_non_trans_tables is not set before Rows_log_event caching
--echo #

# Prove Incident event is written to binlog for any data modifying DML that
# hits an issue at statement-caching for binlog, incl when it's inside
# a stored routine.
# Two execution branches - a single record and multiple records statement -
# are tested.
# Note CREATE..SELECT is not tested here, see MDEV-35207.

--connection default
CREATE TABLE t1w (a TEXT) ENGINE = myisam; /* `w` stands for write as insert */
CREATE TABLE t2d (a TEXT) ENGINE = myisam;
CREATE TABLE t3u (a TEXT) ENGINE = myisam;
CREATE TABLE t1  (a INT)  ENGINE = innodb;
CREATE TABLE t2  (a INT)  ENGINE = innodb;

delimiter |;
CREATE FUNCTION f_25488 ()
RETURNS INTEGER
BEGIN
  INSERT INTO t2 SET a=1;
  DELETE FROM t1w;
RETURN 1;
END |
delimiter ;|

SET STATEMENT binlog_format=STATEMENT FOR INSERT INTO t2d VALUES (REPEAT('a', 4096)),(REPEAT('a', 4096)),(REPEAT('a', 4096));
SET STATEMENT binlog_format=STATEMENT FOR INSERT INTO t3u VALUES (REPEAT('a', 4096)),(REPEAT('a', 4096)),(REPEAT('a', 4096));
call mtr.add_suppression("Write to binary log failed: Multi-row statements required more than 'max_binlog_stmt_cache_size' bytes of storage");

# The issue to hit is a limited size of the statement cache.
SET @max_binlog_stmt_cache_size         = @@global.max_binlog_stmt_cache_size;
SET @binlog_stmt_cache_size             = @@global.binlog_stmt_cache_size;
SET @@global.max_binlog_stmt_cache_size = 4096;
SET @@global.binlog_stmt_cache_size     = 4096;

FLUSH LOGS;

# Incidents with multiple records
connect (con_i,localhost,root,,);
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR INSERT INTO t1w VALUES (REPEAT('a', 4096)),(REPEAT('a', 4096)),(REPEAT('a', 4096));
# use t3u as a source
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR INSERT INTO t1w SELECT * FROM t3u;

connect (con_d,localhost,root,,);
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR DELETE FROM t2d;

connect (con_u,localhost,root,,);
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR UPDATE t3u SET a = REPEAT('@', 4096);

connect (con_f,localhost,root,,);
SET STATEMENT binlog_format=statement FOR INSERT INTO t1w VALUES (REPEAT('a', 4096)),(REPEAT('a', 4096)),(REPEAT('a', 4096));
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR INSERT INTO t1 SET a=f_25488();

# Incident with a single record
connect (con_g,localhost,root,,);
SET STATEMENT binlog_format=statement FOR DELETE FROM t1w;
SET STATEMENT binlog_format=statement FOR INSERT INTO t1w VALUES (REPEAT('a', 3*4096));
--error ER_STMT_CACHE_FULL
SET STATEMENT binlog_format=ROW FOR INSERT INTO t1 SET a=f_25488();


--echo # The proof.
--connection default
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_binlog_events.inc

SET @@global.max_binlog_stmt_cache_size = @max_binlog_stmt_cache_size;
SET @@global.binlog_stmt_cache_size     = @binlog_stmt_cache_size;

DROP TABLE t1w,t2d,t3u,t1,t2;
DROP FUNCTION f_25488;
