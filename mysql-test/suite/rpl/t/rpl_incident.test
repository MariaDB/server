#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

--source include/have_debug.inc
--source include/master-slave.inc

SET @old_binlog_checksum=@@binlog_checksum;
SET GLOBAL BINLOG_CHECKSUM=none;
connection slave;
SET @old_binlog_checksum=@@binlog_checksum;
SET GLOBAL BINLOG_CHECKSUM=none;
connection master;

--echo **** On Master ****
CREATE TABLE t1 (a INT);

INSERT INTO t1 VALUES (1),(2),(3);
SELECT * FROM t1;

set @saved_dbug = @@global.debug_dbug;
SET GLOBAL debug_dbug= '+d,incident_database_resync_on_replace,*';

# This will generate an incident log event and store it in the binary
# log before the replace statement.
REPLACE INTO t1 VALUES (4);
--save_master_pos
SELECT * FROM t1;

set @@global.debug_dbug = @saved_dbug;

connection slave;
# Wait until SQL thread stops with error LOST_EVENT on master
call mtr.add_suppression("Slave SQL.*The incident LOST_EVENTS occurred on the master.* 1590");
let $slave_sql_errno= 1590;
let $show_slave_sql_error= 1;
source include/wait_for_slave_sql_error.inc;

# The 4 should not be inserted into the table, since the incident log
# event should have stop the slave.
--echo **** On Slave ****
SELECT * FROM t1;

SET GLOBAL SQL_SLAVE_SKIP_COUNTER=1;
START SLAVE;
--sync_with_master

# Now, we should have inserted the row into the table and the slave
# should be running. We should also have rotated to a new binary log.

SELECT * FROM t1;
source include/check_slave_is_running.inc;

connection master;
SET GLOBAL BINLOG_CHECKSUM=@old_binlog_checksum;
DROP TABLE t1;
--sync_slave_with_master
SET GLOBAL BINLOG_CHECKSUM=@old_binlog_checksum;
--source include/rpl_end.inc
