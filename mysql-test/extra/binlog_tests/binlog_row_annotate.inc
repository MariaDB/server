#
# This include file is used by more than one test suite
# (currently binlog and binlog_encryption).
# Please check all dependent tests after modifying it
#
# Usage:
# --let use_remote_mysqlbinlog= 1 # optional
# --source extra/binlog_tests/binlog_row_annotate.inc
#
# By default, the script uses mysqlbinlog both with direct access to files
# and via connection to the server. In some cases, direct access to files
# might be impossible (e.g. with encryption). If use_remote_mysqlbinlog
# flag is set, this part of the logic will be omitted.
#

###############################################################################
# WL47: Store in binlog text of statements that caused RBR events
# new event:              ANNOTATE_ROWS_EVENT
# new master option:      --binlog-annotate-row-events
# new mysqlbinlog option: --skip-annotate-row-events
#
# Intended to test that:
# *** If the --binlog-annotate-row-events option is switched on on master
#     then Annotate_rows events:
#     - are generated;
#     - are generated only once for "multi-table-maps" rbr queries;
#     - are not generated when the corresponding queries are filtered away;
#     - are generated when the corresponding queries are filtered away partialy
#       (e.g. in case of multi-delete).
# *** Annotate_rows events are printed by mysqlbinlog started without
#     --skip-annotate-row-events options both in remote and local cases.
# *** Annotate_rows events are not printed by mysqlbinlog started with
#     --skip-annotate-row-events options both in remote and local cases.
###############################################################################

set @old_binlog_checksum=@@binlog_checksum;
set global binlog_checksum=NONE;
--let datadir= `select @@datadir`

--source include/have_log_bin.inc
--source include/binlog_start_pos.inc
--source include/have_binlog_format_row.inc

set sql_mode="";
let $collation_server=`select @@collation_server`;

# Fix timestamp to avoid varying results
SET timestamp=1000000000;

# Delete all existing binary logs
RESET MASTER;

CREATE DATABASE test1;
CREATE TABLE test1.t1(a int);

CREATE DATABASE test2;
CREATE TABLE test2.t2(a int);
CREATE VIEW  test2.v2 AS SELECT * FROM test2.t2;

CREATE DATABASE test3;
CREATE TABLE test3.t3(a int);

CREATE DATABASE xtest1;
CREATE TABLE xtest1.xt1(a int);

CREATE DATABASE xtest2;
CREATE TABLE xtest2.xt2(a int);

# By default SESSION binlog_annotate_row_events = OFF

INSERT INTO test1.t1 VALUES (1), (2), (3);

SET SESSION binlog_annotate_row_events = ON;

INSERT INTO test2.t2 VALUES (1), (2), (3);
INSERT INTO test3.t3 VALUES (1), (2), (3);

# This query generates two Table maps but the Annotate
# event should appear only once before the first Table map
DELETE test1.t1, test2.t2
  FROM test1.t1 INNER JOIN test2.t2 INNER JOIN test3.t3
  WHERE test1.t1.a=test2.t2.a AND test2.t2.a=test3.t3.a;

# This event should be filtered out together with Annotate event
INSERT INTO xtest1.xt1 VALUES (1), (2), (3);

# This event should pass the filter
INSERT INTO test2.v2 VALUES (1), (2), (3);

# This event should pass the filter only for test2.t2 part
DELETE xtest1.xt1, test2.t2
  FROM xtest1.xt1 INNER JOIN test2.t2 INNER JOIN test3.t3
  WHERE xtest1.xt1.a=test2.t2.a AND test2.t2.a=test3.t3.a;

# These events should be filtered out together with Annotate events
INSERT INTO xtest1.xt1 VALUES (1), (2), (3);
INSERT INTO xtest2.xt2 VALUES (1), (2), (3);
DELETE xtest1.xt1, xtest2.xt2
  FROM xtest1.xt1 INNER JOIN xtest2.xt2 INNER JOIN test3.t3
  WHERE xtest1.xt1.a=xtest2.xt2.a AND xtest2.xt2.a=test3.t3.a;

FLUSH LOGS;

--echo #####################################################################################
--echo # The following Annotate_rows events should appear below:
--echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
--echo # - INSERT INTO test3.t3 VALUES (1), (2), (3)
--echo # - DELETE test1.t1, test2.t2 FROM <...>
--echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
--echo # - DELETE xtest1.xt1, test2.t2 FROM <...>
--echo #####################################################################################

--source include/show_binlog_events.inc

if (!$use_remote_mysqlbinlog)
{
    --echo #
    --echo #####################################################################################
    --echo # mysqlbinlog
    --echo # The following Annotates should appear in this output:
    --echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
    --echo # - INSERT INTO test3.t3 VALUES (1), (2), (3)
    --echo # - DELETE test1.t1, test2.t2 FROM <...> (with two subsequent Table maps)
    --echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
    --echo # - DELETE xtest1.xt1, test2.t2 FROM <...> (with one subsequent Table map)
    --echo #####################################################################################

    --replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
    --exec $MYSQL_BINLOG --base64-output=decode-rows -v -v $datadir/master-bin.000001

    --echo #
    --echo #####################################################################################
    --echo # mysqlbinlog --database=test1
    --echo # The following Annotate should appear in this output:
    --echo # - DELETE test1.t1, test2.t2 FROM <...>
    --echo #####################################################################################

    --replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
    --exec $MYSQL_BINLOG --base64-output=decode-rows --database=test1 -v -v $datadir/master-bin.000001

    --echo #
    --echo #####################################################################################
    --echo # mysqlbinlog --skip-annotate-row-events
    --echo # No Annotates should appear in this output
    --echo #####################################################################################

    --replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
    --exec $MYSQL_BINLOG --base64-output=decode-rows --skip-annotate-row-events -v -v $datadir/master-bin.000001

    --let use_remote_mysqlbinlog= 0
}

--echo #
--echo #####################################################################################
--echo # mysqlbinlog --read-from-remote-server
--echo # The following Annotates should appear in this output:
--echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
--echo # - INSERT INTO test3.t3 VALUES (1), (2), (3)
--echo # - DELETE test1.t1, test2.t2 FROM <...> (with two subsequent Table maps)
--echo # - INSERT INTO test2.t2 VALUES (1), (2), (3)
--echo # - DELETE xtest1.xt1, test2.t2 FROM <...> (with one subsequent Table map)
--echo #####################################################################################

--replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
--exec $MYSQL_BINLOG --base64-output=decode-rows -v -v --read-from-remote-server --user=root --host=localhost --port=$MASTER_MYPORT master-bin.000001

--echo #
--echo #####################################################################################
--echo # mysqlbinlog --read-from-remote-server --database=test1
--echo # The following Annotate should appear in this output:
--echo # - DELETE test1.t1, test2.t2 FROM <...>
--echo #####################################################################################

--replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
--exec $MYSQL_BINLOG --base64-output=decode-rows --database=test1 -v -v --read-from-remote-server --user=root --host=localhost --port=$MASTER_MYPORT master-bin.000001

--echo #
--echo #####################################################################################
--echo # mysqlbinlog --read-from-remote-server --skip-annotate-row-events
--echo # No Annotates should appear in this output
--echo #####################################################################################

--replace_regex /server id [0-9]*/server id #/ /server v [^ ]*/server v #.##.##/ /exec_time=[0-9]*/exec_time=#/ /thread_id=[0-9]*/thread_id=#/ /table id [0-9]*/table id #/ /mapped to number [0-9]*/mapped to number #/ /end_log_pos [0-9]*/end_log_pos #/ /# at [0-9]*/# at #/ /collation_server=[0-9]+/collation_server=X/ /character_set_client=[0-9]+/character_set_client=X/ /collation_connection=[0-9]+/collation_connection=X/
--exec $MYSQL_BINLOG --base64-output=decode-rows --skip-annotate-row-events -v -v --read-from-remote-server --user=root --host=localhost --port=$MASTER_MYPORT master-bin.000001

# Clean-up

set global binlog_checksum=@old_binlog_checksum;
DROP DATABASE test1;
DROP DATABASE test2;
DROP DATABASE test3;
DROP DATABASE xtest1;
DROP DATABASE xtest2;
