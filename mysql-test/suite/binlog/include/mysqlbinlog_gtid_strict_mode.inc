#
# This file runs test cases for using --gtid-strict-mode with mariadb-binlog to
# ensure warnings are properly displayed
#
# param $is_strict_mode   boolean (0 for false, 1 for true) to enable or
#                         disable strict mode for GTID processing
#

--let MYSQLD_DATADIR=`select @@datadir`
--let OUT_FILE=$MYSQLTEST_VARDIR/tmp/binlog.out

if ($is_strict_mode == 0)
{
  --let BINLOG_STRICT_MODE_PARAM=--skip-gtid-strict-mode
}
if ($is_strict_mode == 1)
{
  --let BINLOG_STRICT_MODE_PARAM=--gtid-strict-mode
}
if ($is_verbose == 1)
{
  --let BINLOG_STRICT_MODE_PARAM=$BINLOG_STRICT_MODE_PARAM -vvv
}

--let $log_error_ = $MYSQLTEST_VARDIR/tmp/out.err
--let SEARCH_FILE=$log_error_

--echo #
--echo #   Test Case 1:
--echo #   Sequential sequence numbers results in no errors or warnings
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
INSERT INTO t1 values (3);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 2:
--echo #   A skipped sequence number results in no errors or warnings if all
--echo # numbers are monotonic (i.e. gaps in sequence number are allowed
--echo # provided they never decrease)
RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 5;
INSERT INTO t1 values (3);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 3:
--echo #   A sequence number lower than the last processed value results in a
--echo # warning or error
CREATE TABLE t1 (a int);
RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 1;
INSERT INTO t1 values (1);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 4:
--echo #   Skipping a GTID and later receiving it results in a warning or error
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 3;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (3);
SET @@session.gtid_seq_no= 4;
INSERT INTO t1 values (4);
INSERT INTO t1 values (5);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 5:
--echo #   Repeat sequence numbers produce a warning
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 3;
INSERT INTO t1 values (3);
INSERT INTO t1 values (4);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 6:
--echo #   Warnings from different domains are all displayed
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 3;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (3);
SET @@session.gtid_seq_no= 4;
INSERT INTO t1 values (4);
SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 2;
CREATE TABLE t2 (a int);
SET @@session.gtid_seq_no= 3;
INSERT INTO t2 values (2);
SET @@session.gtid_seq_no= 2;
INSERT INTO t2 values (3);
SET @@session.gtid_seq_no= 4;
INSERT INTO t2 values (4);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
DROP TABLE t2;
RESET MASTER;

--echo #
--echo #   Test Case 7:
--echo #   A decreasing seq_no before a start-position is ignored
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
SET @@session.gtid_seq_no= 1;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 3;
INSERT INTO t1 values (1);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (2);
--let $start_binlog_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
SET @@session.gtid_seq_no= 4;
INSERT INTO t1 values (4);
INSERT INTO t1 values (3);
INSERT INTO t1 values (5);
FLUSH LOGS;

--echo # GTID-based start-position
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-4 $BINLOG_STRICT_MODE_PARAM > log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-4 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX
--source include/search_pattern_in_file.inc

--echo # Position-based start-position
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=start_binlog_pos $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=$start_binlog_pos $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX
--source include/search_pattern_in_file.inc

--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 8:
--echo #   A decreasing seq_no inside of a --start/--stop position window is
--echo # displayed
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
--let $start_binlog_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 5;
INSERT INTO t1 values (4);
SET @@session.gtid_seq_no= 4;
INSERT INTO t1 values (3);
SET @@session.gtid_seq_no= 6;
INSERT INTO t1 values (5);
--let $stop_binlog_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
FLUSH LOGS;

--echo # GTID-based window
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --stop-position=0-1-6 $BINLOG_STRICT_MODE_PARAM > log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --stop-position=0-1-6 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc

--echo # Position-based window
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=start_binlog_pos --stop-position=stop_binlog_pos $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=$start_binlog_pos --stop-position=$stop_binlog_pos $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc

--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 9:
--echo #   Error if --stop-position is not greater than or equal to
--echo # --start-position
--echo #
--echo # Note: Error is only displayed in strict mode, -vvv has no effect here
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-2 --stop-position=0-1-1 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-2 --stop-position=0-1-1 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=ERROR: Queried GTID range is invalid in strict mode
--source include/search_pattern_in_file.inc

--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-2,1-2-1 --stop-position=0-1-1,1-2-2 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-2,1-2-1 --stop-position=0-1-1,1-2-2 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=ERROR: Queried GTID range is invalid in strict mode
--source include/search_pattern_in_file.inc
RESET MASTER;

--echo #
--echo #   Test Case 10:
--echo #   Strict mode warnings should be independent of --offset option
--echo # specification
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
SET @@session.gtid_seq_no= 1;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 3;
INSERT INTO t1 values (1);
SET @@session.gtid_seq_no= 2;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 4;
INSERT INTO t1 values (3);
INSERT INTO t1 values (4);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --offset=8 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --offset=8 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 11:
--echo #   Strict mode warnings should be independent of --start-timestamp
--echo # option specification
set @a=UNIX_TIMESTAMP("1970-01-21 15:32:22");
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
SET @@session.gtid_seq_no= 1;
SET timestamp=@a;
CREATE TABLE t1 (a int);
SET @@session.gtid_seq_no= 3;
SET timestamp=@a+1;
INSERT INTO t1 values (1);
SET @@session.gtid_seq_no= 2;
SET timestamp=@a+2;
INSERT INTO t1 values (2);
SET @@session.gtid_seq_no= 4;
SET timestamp=@a+3;
INSERT INTO t1 values (3);
INSERT INTO t1 values (4);
FLUSH LOGS;
--echo # MYSQL_BINLOG MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --start-datetime="1970-01-21 15:32:24" $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--error $is_strict_mode
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/master-bin.000001 --start-position=0-1-1 --start-datetime="1970-01-21 15:32:24" $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=$DEFAULT_ERROR_PREFIX: Found out of order GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
RESET MASTER;

--echo #
--echo #   Test Case 12:
--echo #   Specifying multiple binary logs with a log-position start should
--echo # skip GTID state verification
RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
INSERT INTO t1 values (1);
--let $b2_start_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 2;
CREATE TABLE t2 (a int);
INSERT INTO t2 values (1);
FLUSH LOGS;
INSERT INTO t2 values (2);
FLUSH LOGS;
--let BINLOG_FILE1= query_get_value(SHOW BINARY LOGS, Log_name, 1)
# Skip file 2 as input
--let BINLOG_FILE2= query_get_value(SHOW BINARY LOGS, Log_name, 2)
--echo # MYSQL_BINLOG MYSQLD_DATADIR/BINLOG_FILE1 MYSQLD_DATADIR/BINLOG_FILE2 --start-position=b2_start_pos $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE1 $MYSQLD_DATADIR/$BINLOG_FILE2 --start-position=$b2_start_pos $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
DROP TABLE t1;
DROP TABLE t2;


--echo #
--echo #   Test Case 13:
--echo #   If multiple binary logs should be specified but a middle log is
--echo # missing, we should detect that and warn when using -vvv

RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
FLUSH LOGS;
INSERT INTO t1 values (1);
--let $b2_start_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 2;
CREATE TABLE t2 (a int);
FLUSH LOGS;
SET @@session.gtid_domain_id= 2;
SET @@session.server_id= 3;
CREATE TABLE t3 (a int);
FLUSH LOGS;

--echo #
--echo # GLLE from each log for state reference
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 1)
--source include/show_gtid_list.inc
--let $binlog_file=
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 2)
--source include/show_gtid_list.inc
--let $binlog_file=
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 3)
--source include/show_gtid_list.inc
--let $binlog_file=

--let BINLOG_FILE1= query_get_value(SHOW BINARY LOGS, Log_name, 1)
# Skip file 2 as input
--let BINLOG_FILE3= query_get_value(SHOW BINARY LOGS, Log_name, 3)
--echo # MYSQL_BINLOG MYSQLD_DATADIR/BINLOG_FILE1 MYSQLD_DATADIR/BINLOG_FILE3 $BINLOG_STRICT_MODE_PARAM 2> log_error_ > OUT_FILE
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE1 $MYSQLD_DATADIR/$BINLOG_FILE3 $BINLOG_STRICT_MODE_PARAM 2> $log_error_ > $OUT_FILE
--echo # We should have two warnings about missing data from domains 0 and 1 if
--echo # -vvv is specified
--let SEARCH_FILE=$log_error_
--let SEARCH_PATTERN=WARNING: Binary logs are missing data for domain 0[^\n]+the last seen event was
--source include/search_pattern_in_file.inc
--let SEARCH_PATTERN=WARNING: Binary logs are missing data for domain 1[^\n]+neither the starting GTID position list nor any processed events
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
RESET MASTER;

--echo #
--echo #   Test Case 14:
--echo #   If a --stop-position GTID occurs before the first specified binlog's
--echo # GLLE, error
RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
FLUSH LOGS;
INSERT INTO t1 values (3);
FLUSH LOGS;

--echo #
--echo # GLLE from each log for state reference
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 1)
--source include/show_gtid_list.inc
--let $binlog_file=
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 2)
--source include/show_gtid_list.inc
--let $binlog_file=
--echo # MYSQL_BINLOG MYSQLD_DATADIR/BINLOG_FILE2 $BINLOG_STRICT_MODE_PARAM --stop-position=0-1-2 2> log_error_ > OUT_FILE
--error 1
--exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE2 $BINLOG_STRICT_MODE_PARAM --stop-position=0-1-2 2> $log_error_ > $OUT_FILE
--let SEARCH_PATTERN=ERROR: --stop-position GTID
--source include/search_pattern_in_file.inc
--remove_file $log_error_
DROP TABLE t1;

--remove_file $OUT_FILE
