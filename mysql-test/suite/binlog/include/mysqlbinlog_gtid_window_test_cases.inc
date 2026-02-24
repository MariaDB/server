#
# This file runs test cases for providing GTIDs to --start-position and
# --stop-position arguments in mariadb-binlog
#
# param $is_remote   boolean (0 for false, 1 for true) to perform a local file
#                    or remote host analysis
#

--let MYSQLD_DATADIR=`select @@datadir`
--let data_inconsistent_err= "table data is inconsistent after replaying binlog using GTID start/stop positions";
--let $tmp_out_ = $MYSQLTEST_VARDIR/tmp/null.log

## Initialize test data
#
set @a=UNIX_TIMESTAMP("1970-01-21 15:32:22");
SET timestamp=@a;
RESET MASTER;
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
--let $empty_binlog_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
CREATE TABLE t1 (a int);
SET timestamp=@a+1;
INSERT INTO t1 values (1), (2);
--let test2_t1_mid_checksum= `CHECKSUM TABLE t1`

SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 2;
SET timestamp=@a+2;
CREATE TABLE t2 (a int);
SET timestamp=@a+3;
INSERT INTO t2 values (1);
--let t2_mid_checksum= `CHECKSUM TABLE t2`

SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
SET timestamp=@a+4;
INSERT INTO t1 values (3), (4);
--let t1_final_checksum_pos= query_get_value(SHOW MASTER STATUS,Position, 1)
--let t1_final_checksum= `CHECKSUM TABLE t1`

SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 2;
INSERT INTO t2 values (2);
--let test4_t2_good_checksum= `CHECKSUM TABLE t2`

SET @@session.server_id= 3;
INSERT INTO t2 values (3);
--let test3_t2_good_checksum= `CHECKSUM TABLE t2`

SET @@session.server_id= 2;
INSERT INTO t2 values (4);
--let t2_final_checksum= `CHECKSUM TABLE t2`

FLUSH LOGS;

# Multiple binlog file case, used by test 18
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t3 (a int);
INSERT INTO t3 VALUES (1);
--let t3_final_checksum= `CHECKSUM TABLE t3`

FLUSH LOGS;

--echo # Gtid list event of the 2nd binlog file whose content is
--echo # matched against --start-position in the following tests:
--let $binlog_file= query_get_value(SHOW BINARY LOGS, Log_name, 2)
--source include/show_gtid_list.inc
--let $binlog_file=

--let BINLOG_FILE= query_get_value(SHOW BINARY LOGS, Log_name, 1)
--let BINLOG_FILE2= query_get_value(SHOW BINARY LOGS, Log_name, 2)

if ($is_remote == 0)
{
    --let BINLOG_FILE_PARAM=$MYSQLD_DATADIR/$BINLOG_FILE.orig
    --let BINLOG_FILE_PARAM2=$MYSQLD_DATADIR/$BINLOG_FILE2.orig
}
if ($is_remote == 1)
{
    --let BINLOG_FILE_PARAM= --read-from-remote-server $BINLOG_FILE
    --let BINLOG_FILE_PARAM2= --read-from-remote-server $BINLOG_FILE2
}

--copy_file $MYSQLD_DATADIR/$BINLOG_FILE $MYSQLD_DATADIR/$BINLOG_FILE.orig
--copy_file $MYSQLD_DATADIR/$BINLOG_FILE2 $MYSQLD_DATADIR/$BINLOG_FILE2.orig

DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;


--echo #
--echo #   Test Case 1:
--echo #   The end of the binlog file resets the server and domain id of the
--echo # session

# As gtid_domain_id and server_id should not change after reading binlog in GTID
# mode, change variables to otherwise-unused values to ensure they remain
--let $reset_gtid_domain_id = `select @@session.gtid_domain_id`
--let $reset_server_id = `select @@session.server_id`
SET @@session.gtid_domain_id= 10;
SET @@session.server_id= 20;

# Replay the binlog
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 | $MYSQL
--let $test_gtid_domain_id = `select @@session.gtid_domain_id`

# Ensure variables haven't changed
--let $assert_text = session gtid_domain_id should not change when reading binlog in GTID mode
--let $assert_cond = @@session.gtid_domain_id = 10
--source include/assert.inc
--let $assert_text = session server_id should not change when reading binlog in GTID mode
--let $assert_cond = @@session.server_id = 20
--source include/assert.inc

# Reset back to previous state
--eval SET @@session.gtid_domain_id= $reset_gtid_domain_id
--eval SET @@session.server_id= $reset_server_id
DROP TABLE t1;


--echo #
--echo #   Test Case 2:
--echo #   Single GTID range specified
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 | $MYSQL

if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;


--echo #
--echo #   Test Case 3:
--echo #   Single GTID range with different server_ids
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=1-2-0 --stop-position=1-3-4 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=1-2-0 --stop-position=1-3-4 | $MYSQL

if ($test3_t2_good_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude domain 0 from stop position";
}
DROP TABLE t2;


--echo #
--echo #   Test Case 4:
--echo #   Multiple GTID ranges specified
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0,1-2-0 --stop-position=0-1-3,1-2-3 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0,1-2-0 --stop-position=0-1-3,1-2-3 | $MYSQL

# Reuse checksum spot from test 4
if ($t1_final_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if ($test4_t2_good_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;
DROP TABLE t2;


--echo #
--echo #   Test Case 5:
--echo #   Multiple GTID ranges specified where the domain ids are listed in
--echo # different orders between start/stop position
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=0-1-3,1-2-3 --start-position=1-2-0,0-1-0 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=0-1-3,1-2-3 --start-position=1-2-0,0-1-0 | $MYSQL

# Reuse checksum spot from test 4
if ($t1_final_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if ($test4_t2_good_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;
DROP TABLE t2;


--echo #
--echo #   Test Case 6:
--echo #   Only start position specified
CREATE TABLE t1 (a int);
INSERT INTO t1 values (3), (4);
--let test6_t1_mid_checksum= `CHECKSUM TABLE t1`
DROP TABLE t1;
CREATE TABLE t1 (a int);
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-2 | $MYSQL
if ($test6_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if ($t2_final_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;
DROP TABLE t2;


--echo #
--echo #   Test Case 7:
--echo #   Only stop position specified
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=0-1-2 | $MYSQL

# Reuse checksum spot from test 2
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;


--echo #
--echo #   Test Case 8:
--echo #   Seq_no=0 in --start-position includes all events for a domain
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0,1-2-0 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0,1-2-0 | $MYSQL
if ($t1_final_checksum != `CHECKSUM TABLE t1`)
{
    die "t1 data should be complete as binlog replay should include domain 0 entirely in results";
}
if ($t2_final_checksum != `CHECKSUM TABLE t2`)
{
    die "t2 data should be complete as binlog replay should include domain 1 entirely in results";
}
DROP TABLE t1;
DROP TABLE t2;

--echo #
--echo #   Test Case 9:
--echo #   Seq_no=0 in --stop-position excludes all events for a domain
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=0-1-0,1-2-0 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=0-1-0,1-2-0 | $MYSQL
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude domain 0 from results";
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from results";
}

--echo #
--echo #   Test Case 10:
--echo #   Output stops for all domain ids when all --stop-position GTID values
--echo # have been hit.
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=0-1-2 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from results";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 11:
--echo #   All GTID events from other domains are printed until the
--echo # --stop-position values are hit
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=1-3-4 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=1-3-4 | $MYSQL
if ($test3_t2_good_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude domain 0 from stop position";
}
DROP TABLE t2;

--echo #
--echo #   Test Case 12:
--echo #   Scalar and GTID values can be used together for stop or start
--echo # position
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=empty_binlog_pos --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=$empty_binlog_pos --stop-position=0-1-2 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=t1_final_checksum_pos | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=$t1_final_checksum_pos | $MYSQL
if ($t1_final_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if ($t2_mid_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
DROP TABLE t1;
DROP TABLE t2;

--echo #
--echo #   Test Case 13:
--echo #   If the start position is delayed within the binlog, events occurring
--echo # before that position are ignored
CREATE TABLE t1 (a int);
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-1 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-1 --stop-position=0-1-2 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from results";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 14:
--echo #   If start position is repeated, the last specification overrides all
--echo # previous ones
CREATE TABLE t1 (a int);
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --start-position=0-1-1 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --start-position=0-1-1 --stop-position=0-1-2 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from results";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 15:
--echo #   If stop position is repeated, the last specification overrides all
--echo # previous ones
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-1 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-1 --stop-position=0-1-2 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from results";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 16:
--echo #   Start position with --offset=<n> skips n events after the first
--echo # GTID is found

# t1 needs to be specified because its creation should be skipped from
# --offset specification
CREATE TABLE t1 (a int);

--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 --offset=5 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=0-1-2 --offset=5 | $MYSQL
if ($test2_t1_mid_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude events after GTID 0-1-2";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 17:
--echo #   Start position with --start-datetime=<T> where T occurs after the
--echo # specified GTID results in no events before T
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=1-2-2 --start-datetime="1970-01-21 15:32:24" | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-0 --stop-position=1-2-2 --start-datetime="1970-01-21 15:32:24" | $MYSQL
if ($t2_mid_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude events before the given start-datetime";
}
DROP TABLE t2;

--echo #
--echo #   Test Case 18:
--echo #   If --stop-position is specified, domains which are not present
--echo # in its list should be excluded from output
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --stop-position=1-3-4 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --stop-position=1-3-4 | $MYSQL

if ($test3_t2_good_checksum != `CHECKSUM TABLE t2`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude domain 0 from stop position";
}
DROP TABLE t2;

--echo #
--echo #   Test Case 19:
--echo #   If the start and stop GTIDs in any domain are equal, the domain
--echo # should not have any output
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=0-1-2 --stop-position=0-1-2 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=0-1-2 --stop-position=0-1-2 | $MYSQL

if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should exclude domain 0 from stop position";
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from stop position";
}

--echo #
--echo #   Test Case 20:
--echo #   If --start-position and --stop-position have different domain ids,
--echo # only events from GTIDs in the --stop-position list are output
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM --start-position=1-2-2 --stop-position=0-1-3 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM --start-position=1-2-2 --stop-position=0-1-3 | $MYSQL
if ($t1_final_checksum != `CHECKSUM TABLE t1`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should exclude domain 1 from stop position";
}
DROP TABLE t1;

--echo #
--echo #   Test Case 21:
--echo #   Successive binary logs (e.g. logs with previous logs that have been
--echo # purged) will write events when the --start-position matches their
--echo # Gtid_list_log_event state
if ($is_remote == 1)
{
    --echo #
    --echo # Reset server state
    RESET MASTER;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE.orig | $MYSQL
    FLUSH LOGS;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE2.orig | $MYSQL
    FLUSH LOGS;
    --eval PURGE BINARY LOGS TO "$BINLOG_FILE2"
    DROP TABLE t1;
    DROP TABLE t2;
    DROP TABLE t3;
}
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM2 --start-position=0-1-3,1-2-5 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM2 --start-position=0-1-3,1-2-5 | $MYSQL
if ($t3_final_checksum != `CHECKSUM TABLE t3`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should not have any events from unspecified binlog file";
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should not have any events from unspecified binlog file";
}
DROP TABLE t3;

--echo #
--echo #   Test Case 22:
--echo #   Successive binary logs can be called with --stop-position and
--echo # without --start-position
if ($is_remote == 1)
{
    --echo #
    --echo # Reset server state
    RESET MASTER;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE.orig | $MYSQL
    FLUSH LOGS;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE2.orig | $MYSQL
    FLUSH LOGS;
    --eval PURGE BINARY LOGS TO "$BINLOG_FILE2"
    DROP TABLE t1;
    DROP TABLE t2;
    DROP TABLE t3;
}
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM2 --stop-position=0-1-4 | MYSQL
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM2 --stop-position=0-1-4 | $MYSQL
if (`SELECT COUNT(*) FROM test.t3`)
{
    die $data_inconsistent_err;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`)
{
    die "t1 should not exist as binlog replay should not have any events from unspecified binlog file";
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`)
{
    die "t2 should not exist as binlog replay should not have any events from unspecified binlog file";
}
DROP TABLE t3;


if ($is_remote == 1)
{
    --echo #
    --echo # Remote-only setup for error cases
    RESET MASTER;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE.orig | $MYSQL
    FLUSH LOGS;
    --exec $MYSQL_BINLOG $MYSQLD_DATADIR/$BINLOG_FILE2.orig | $MYSQL
    FLUSH LOGS;
    --eval PURGE BINARY LOGS TO "$BINLOG_FILE2"
}

--echo # At the following error cases analysis
--echo # note incompatible --start-position with the value of
--echo # Gtid list event of the 2nd binlog file printed above.

--let err_out_= $MYSQLTEST_VARDIR/tmp/err.out
--let SEARCH_FILE=$err_out_

--echo #
--echo #   Error Case 1:
--echo #    A GTID --start-position that does not mention all domains that make
--echo # up the binary log state should error
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM2 --start-position=0-1-3 > tmp_out_ 2> err_out_
--error 1
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM2 --start-position=0-1-3 > $tmp_out_ 2> $err_out_
if ($is_remote == 1)
{
    --let SEARCH_PATTERN=ERROR: Got error reading packet from server
}
if ($is_remote == 0)
{
    --let SEARCH_PATTERN=ERROR: Starting GTID position list does not specify an initial value
}
--source include/search_pattern_in_file.inc
--remove_file $err_out_

--echo #
--echo #   Error Case 2:
--echo #   A GTID --start-position with any sequence numbers which occur before
--echo # the binary log state should result in error
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM2 --start-position=0-1-2,1-2-5 > tmp_out_ 2> err_out_
--error 1
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM2 --start-position=0-1-2,1-2-5 > $tmp_out_ 2> $err_out_
if ($is_remote == 1)
{
    --let SEARCH_PATTERN=ERROR: Got error reading packet from server
}
if ($is_remote == 0)
{
    --let SEARCH_PATTERN=ERROR: Binary logs are missing data for domain 0
}
--source include/search_pattern_in_file.inc
--remove_file $err_out_

--echo #
--echo #   Error Case 3:
--echo #   A GTID --start-position with any sequence numbers that are not
--echo # eventually processed results in error
--echo # MYSQL_BINLOG BINLOG_FILE_PARAM2 --start-position=0-1-8,1-2-6 > tmp_out_ 2> err_out_
--error 1
--exec $MYSQL_BINLOG $BINLOG_FILE_PARAM2 --start-position=0-1-8,1-2-6 > $tmp_out_ 2> $err_out_
if ($is_remote == 1)
{
    --let SEARCH_PATTERN=ERROR: Got error reading packet from server
}
if ($is_remote == 0)
{
    --let SEARCH_PATTERN=ERROR: Binary logs never reached expected GTID state
}
--source include/search_pattern_in_file.inc
--remove_file $err_out_

if ($is_remote == 1)
{
    --echo #
    --echo # Remote-only cleanup from error cases
    DROP TABLE t1;
    DROP TABLE t2;
    DROP TABLE t3;
    RESET MASTER;
}

--remove_file $MYSQLD_DATADIR/$BINLOG_FILE.orig
--remove_file $MYSQLD_DATADIR/$BINLOG_FILE2.orig
--remove_file $tmp_out_
