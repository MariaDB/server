#
# This include file is used by more than one test suite
# (currently binlog and binlog_encryption).
# Please check all dependent tests after modifying it
#

source include/have_debug.inc;
source include/have_innodb.inc;
source include/have_log_bin.inc;
source include/have_binlog_format_mixed_or_statement.inc;

CALL mtr.add_suppression("Error writing file 'master-bin'");

RESET MASTER;

CREATE TABLE t1 (a INT PRIMARY KEY) ENGINE=innodb;
INSERT INTO t1 VALUES(0);
SET @saved_dbug = @@SESSION.debug_dbug;
SET SESSION debug_dbug='+d,fail_binlog_write_1';
# The error injection is in the "legacy" code path.
SET GLOBAL binlog_legacy_event_pos= 1;
--error ER_ERROR_ON_WRITE
INSERT INTO t1 VALUES(1);
--error ER_ERROR_ON_WRITE
INSERT INTO t1 VALUES(2);
SET SESSION debug_dbug=@saved_dbug;
SET GLOBAL binlog_legacy_event_pos= 0;
INSERT INTO t1 VALUES(3);
SELECT * FROM t1;

# Actually the output from this currently shows a bug.
# The injected IO error leaves partially written transactions in the binlog in
# the form of stray "BEGIN" events.
# These should disappear from the output if binlog error handling is improved
# (see MySQL Bug#37148 and WL#1790).
--replace_regex /\/\* xid=.* \*\//\/* XID *\// /Server ver: .*, Binlog ver: .*/Server ver: #, Binlog ver: #/ /table_id: [0-9]+/table_id: #/
--replace_column 1 BINLOG 2 POS 5 ENDPOS
SHOW BINLOG EVENTS;

DROP TABLE t1;
