#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

--source include/have_innodb.inc
--let $rpl_topology=1->2->3->4
--source include/rpl_init.inc

# Set up a 4-deep replication topology, then test various fail-overs
# using GTID.
#
#  A -> B -> C -> D

connection server_1;
--source include/wait_for_binlog_checkpoint.inc
--let $binlog_file = query_get_value(SHOW MASTER STATUS,File,1)
--let $binlog_pos = query_get_value(SHOW MASTER STATUS,Position,1)
--echo *** GTID position should be empty here ***
--replace_result $binlog_file <BINLOG_FILE> $binlog_pos <BINLOG_POS> 
eval SELECT BINLOG_GTID_POS('$binlog_file',$binlog_pos);

CREATE TABLE t1 (a INT PRIMARY KEY, b VARCHAR(10)) ENGINE=MyISAM;
CREATE TABLE t2 (a INT PRIMARY KEY, b VARCHAR(10)) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1, "m1");
INSERT INTO t1 VALUES (2, "m2"), (3, "m3"), (4, "m4");
INSERT INTO t2 VALUES (1, "i1");
BEGIN;
INSERT INTO t2 VALUES (2, "i2"), (3, "i3");
INSERT INTO t2 VALUES (4, "i4");
COMMIT;
save_master_pos;
source include/wait_for_binlog_checkpoint.inc;
--let $binlog_file = query_get_value(SHOW MASTER STATUS,File,1)
--let $binlog_pos = query_get_value(SHOW MASTER STATUS,Position,1)
--let $gtid_pos_server_1 = `SELECT @@gtid_binlog_pos`
--echo *** GTID position should be non-empty here ***
--replace_result $binlog_file <BINLOG_FILE> $binlog_pos <BINLOG_POS> $gtid_pos_server_1 <GTID_POS_SERVER_1>
eval SELECT BINLOG_GTID_POS('$binlog_file',$binlog_pos);

connection server_2;
sync_with_master;
source include/wait_for_binlog_checkpoint.inc;
--let $binlog_file = query_get_value(SHOW MASTER STATUS,File,1)
--let $binlog_pos = query_get_value(SHOW MASTER STATUS,Position,1)
--echo *** GTID position should be the same as on server_1 ***
--replace_result $binlog_file <BINLOG_FILE> $binlog_pos <BINLOG_POS> $gtid_pos_server_1 <GTID_POS_SERVER_1>
eval SELECT BINLOG_GTID_POS('$binlog_file',$binlog_pos);
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;
save_master_pos;

connection server_3;
sync_with_master;
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;
save_master_pos;

connection server_4;
sync_with_master;
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;


--echo *** Now take out D, let it fall behind a bit, and then test re-attaching it to A ***
connection server_4;
--source include/stop_slave.inc

connection server_1;
INSERT INTO t1 VALUES (5, "m1a");
INSERT INTO t2 VALUES (5, "i1a");
save_master_pos;

connection server_4;
--replace_result $MASTER_MYPORT MASTER_PORT
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $MASTER_MYPORT,
     MASTER_USE_GTID=CURRENT_POS;
--source include/start_slave.inc
sync_with_master;
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;

--echo *** Now move B to D (C is still replicating from B) ***
connection server_2;
--source include/stop_slave.inc
--replace_result $SERVER_MYPORT_4 SERVER_MYPORT_4
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $SERVER_MYPORT_4,
     MASTER_USE_GTID=CURRENT_POS;
--source include/start_slave.inc

connection server_4;
UPDATE t2 SET b="j1a" WHERE a=5;
save_master_pos;

connection server_2;
sync_with_master;
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;

--echo *** Now move C to D, after letting it fall a little behind ***
connection server_3;
--source include/stop_slave.inc

connection server_1;
INSERT INTO t2 VALUES (6, "i6b");
INSERT INTO t2 VALUES (7, "i7b");
--source include/save_master_gtid.inc

connection server_3;
--replace_result $SERVER_MYPORT_4 SERVER_MYPORT_4
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $SERVER_MYPORT_4,
     MASTER_USE_GTID=CURRENT_POS;
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
SELECT * FROM t2 ORDER BY a;

--echo *** Now change everything back to what it was, to make rpl_end.inc happy
# Also check that MASTER_USE_GTID=CURRENT_POS is still enabled.
connection server_2;
# We need to sync up server_2 before switching. If it happened to have reached
# the point 'UPDATE t2 SET b="j1a" WHERE a=5' it will fail to connect to
# server_1, which is (deliberately) missing that transaction.
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
--replace_result $MASTER_MYPORT MASTER_MYPORT
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $MASTER_MYPORT;
--source include/start_slave.inc
--source include/wait_for_slave_to_start.inc

connection server_3;
--source include/stop_slave.inc
--replace_result $SLAVE_MYPORT SLAVE_MYPORT
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $SLAVE_MYPORT;
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

connection server_4;
--source include/stop_slave.inc
--replace_result $SERVER_MYPORT_3 SERVER_MYPORT_3
eval CHANGE MASTER TO master_host = '127.0.0.1', master_port = $SERVER_MYPORT_3;
--source include/start_slave.inc

connection server_1;
DROP TABLE t1,t2;
--source include/save_master_gtid.inc

--echo *** A few more checks for BINLOG_GTID_POS function ***
--let $valid_binlog_name = query_get_value(SHOW BINARY LOGS,Log_name,1)
--error ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT
SELECT BINLOG_GTID_POS();
--error ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT
SELECT BINLOG_GTID_POS('a');
--error ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT
SELECT BINLOG_GTID_POS('a',1,NULL);
SELECT BINLOG_GTID_POS(1,'a');
SELECT BINLOG_GTID_POS(NULL,NULL);
SELECT BINLOG_GTID_POS('',1);
SELECT BINLOG_GTID_POS('a',1);
eval SELECT BINLOG_GTID_POS('$valid_binlog_name',-1);
eval SELECT BINLOG_GTID_POS('$valid_binlog_name',0);
eval SELECT BINLOG_GTID_POS('$valid_binlog_name',18446744073709551615);
eval SELECT BINLOG_GTID_POS('$valid_binlog_name',18446744073709551616);


--echo *** Some tests of @@GLOBAL.gtid_binlog_state ***
--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc

--connection server_1
SET @old_state= @@GLOBAL.gtid_binlog_state;

--error ER_BINLOG_MUST_BE_EMPTY
SET GLOBAL gtid_binlog_state = '';
RESET MASTER;
SET GLOBAL gtid_binlog_state = '';
FLUSH LOGS;
--source include/show_binary_logs.inc
SET GLOBAL gtid_binlog_state = '0-1-10,1-2-20,0-3-30';
--source include/show_binary_logs.inc
--let $binlog_file= master-bin.000001
--let $binlog_start= 4
--source include/show_binlog_events.inc
#SELECT @@GLOBAL.gtid_binlog_pos;
#SELECT @@GLOBAL.gtid_binlog_state;
--error ER_BINLOG_MUST_BE_EMPTY
SET GLOBAL gtid_binlog_state = @old_state;
RESET MASTER;
SET GLOBAL gtid_binlog_state = @old_state;

# Check that slave can reconnect again, despite the RESET MASTER, as we
# restored the state.

CREATE TABLE t1 (a INT PRIMARY KEY);
SET gtid_seq_no=100;
INSERT INTO t1 VALUES (1);
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
# We cannot just use sync_with_master as we've done RESET MASTER, so
# slave old-style position is wrong.
# So sync on gtid position instead.
--source include/sync_with_master_gtid.inc

SELECT * FROM t1;
# Check that the IO gtid position in SHOW SLAVE STATUS is also correct.
--let $status_items= Gtid_IO_Pos
--source include/show_slave_status.inc

--echo *** Test @@LAST_GTID and MASTER_GTID_WAIT() ***

--connection server_1
DROP TABLE t1;
CREATE TABLE t1 (a INT PRIMARY KEY) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc

--connect (m1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SELECT @@last_gtid;
SET gtid_seq_no=110;
SELECT @@last_gtid;
BEGIN;
SELECT @@last_gtid;
INSERT INTO t1 VALUES (2);
SELECT @@last_gtid;
COMMIT;
SELECT @@last_gtid;
--let $pos= `SELECT @@gtid_binlog_pos`

--connect (s1,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
eval SET @pos= '$pos';
# Check NULL argument.
SELECT master_gtid_wait(NULL);
# Check empty argument returns immediately.
SELECT master_gtid_wait('', NULL);
# Check this gets counted
SHOW STATUS LIKE 'Master_gtid_wait_count';
SHOW STATUS LIKE 'Master_gtid_wait_timeouts';
SHOW STATUS LIKE 'Master_gtid_wait_time';
# Let's check that we get a timeout
SELECT master_gtid_wait(@pos, 0.5);
SELECT * FROM t1 ORDER BY a;
# Now actually wait until the slave reaches the position
send SELECT master_gtid_wait(@pos);

--connection server_2
--source include/start_slave.inc

--connection s1
reap;
SELECT * FROM t1 ORDER BY a;

# Test waiting on a domain that does not exist yet.
--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id= 1;
INSERT INTO t1 VALUES (3);
--let $pos= `SELECT @@gtid_binlog_pos`

--connection s1
--replace_result $pos POS
eval SET @pos= '$pos';
SELECT master_gtid_wait(@pos, 0);
SELECT * FROM t1 WHERE a >= 3;
send SELECT master_gtid_wait(@pos, -1);

--connection server_2
--source include/start_slave.inc

--connection s1
reap;
SELECT * FROM t1 WHERE a >= 3;
# Waiting for only part of the position.
SELECT master_gtid_wait('1-1-1', 0);

# Now test a lot of parallel master_gtid_wait() calls, completing in different
# order, and some of which time out or get killed on the way.

--connection s1
send SELECT master_gtid_wait('2-1-1,1-1-4,0-1-110');

--connect (s2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
# This will time out. No event 0-1-1000 exists
send SELECT master_gtid_wait('0-1-1000', 0.5);

--connect (s3,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
# This one we will kill
--let $kill1_id= `SELECT connection_id()`
send SELECT master_gtid_wait('0-1-2000');

--connect (s4,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('2-1-10');

--connect (s5,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('2-1-6', 1);

# This one we will kill also.
--connect (s6,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
--let $kill2_id= `SELECT connection_id()`
send SELECT master_gtid_wait('2-1-5');

--connect (s7,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('2-1-10');

--connect (s8,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('2-1-5,1-1-4,0-1-110');

--connect (s9,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('2-1-2');

--connection server_2
# This one completes immediately.
SHOW STATUS LIKE 'Master_gtid_wait_timeouts';
SHOW STATUS LIKE 'Master_gtid_wait_count';
SELECT master_gtid_wait('1-1-1');
SHOW STATUS LIKE 'Master_gtid_wait_timeouts';
SHOW STATUS LIKE 'Master_gtid_wait_count';
let $wait_time = query_get_value(SHOW STATUS LIKE 'Master_gtid_wait_time', Value, 1);
--replace_result $wait_time MASTER_GTID_WAIT_TIME
eval SET @a= $wait_time;
SELECT IF(@a <= 100*1000*1000, "OK", CONCAT("Error: wait time ", @a, " is larger than expected"))
  AS Master_gtid_wait_time_as_expected;


--connect (s10,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
send SELECT master_gtid_wait('0-1-109');

--connection server_2
# This one should time out.
SHOW STATUS LIKE 'Master_gtid_wait_timeouts';
SHOW STATUS LIKE 'Master_gtid_wait_count';
SELECT master_gtid_wait('2-1-2', 0.5);
SHOW STATUS LIKE 'Master_gtid_wait_timeouts';
SHOW STATUS LIKE 'Master_gtid_wait_count';
let $wait_time = query_get_value(SHOW STATUS LIKE 'Master_gtid_wait_time', Value, 1);
--replace_result $wait_time MASTER_GTID_WAIT_TIME
eval SET @a= $wait_time;
# We expect a wait time of just a bit over 0.5 seconds. But thread scheduling
# and timer inaccuracies could introduce significant jitter. So allow a
# generous interval.
SELECT IF(@a BETWEEN 0.4*1000*1000 AND 100*1000*1000, "OK", CONCAT("Error: wait time ", @a, " not as expected")) AS Master_gtid_wait_time_as_expected;

--replace_result $kill1_id KILL_ID
eval KILL QUERY $kill1_id;
--connection s3
--error ER_QUERY_INTERRUPTED
reap;

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=2;
INSERT INTO t1 VALUES (4);

--connection s9
reap;

--connection server_2
--replace_result $kill2_id KILL_ID
eval KILL CONNECTION $kill2_id;

--connection s6
--error 2013,ER_CONNECTION_KILLED
reap;

--connection server_1
SET gtid_domain_id=1;
SET gtid_seq_no=4;
INSERT INTO t1 VALUES (5);
SET gtid_domain_id=2;
SET gtid_seq_no=5;
INSERT INTO t1 VALUES (6);

--connection s8
reap;
--connection s1
reap;
--connection s2
reap;
--connection s5
reap;
--connection s10
reap;

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=10;
INSERT INTO t1 VALUES (7);

--connection s4
reap;
--connection s7
reap;


--echo *** Test gtid_slave_pos when used with GTID ***

--connection server_2
--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=1000;
INSERT INTO t1 VALUES (10);
INSERT INTO t1 VALUES (11);
--save_master_pos

--connection server_2
SET sql_slave_skip_counter= 1;
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 10 ORDER BY a;
SELECT IF(LOCATE("2-1-1001", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 2-1-1001 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=1010;
INSERT INTO t1 VALUES (12);
INSERT INTO t1 VALUES (13);
--save_master_pos

--connection server_2
SET sql_slave_skip_counter= 2;
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 10 ORDER BY a;
SELECT IF(LOCATE("2-1-1011", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 2-1-1011 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=1020;
INSERT INTO t1 VALUES (14);
INSERT INTO t1 VALUES (15);
INSERT INTO t1 VALUES (16);
--save_master_pos

--connection server_2
SET sql_slave_skip_counter= 3;
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 10 ORDER BY a;
SELECT IF(LOCATE("2-1-1022", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 2-1-1022 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id=2;
SET gtid_seq_no=1030;
# Disable logging Annotate_rows events to preserve events count.
let $binlog_annotate_row_events_saved= `SELECT @@binlog_annotate_row_events`;
SET @@binlog_annotate_row_events= 0;
INSERT INTO t1 VALUES (17);
INSERT INTO t1 VALUES (18);
INSERT INTO t1 VALUES (19);
eval SET @@binlog_annotate_row_events= $binlog_annotate_row_events_saved;
--save_master_pos

--connection server_2
SET sql_slave_skip_counter= 5;
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 10 ORDER BY a;
SELECT IF(LOCATE("2-1-1032", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 2-1-1032 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;


--source include/stop_slave.inc

--connection server_1
SET gtid_domain_id=3;
SET gtid_seq_no=100;
CREATE TABLE t2 (a INT PRIMARY KEY);
DROP TABLE t2;
SET gtid_domain_id=2;
SET gtid_seq_no=1040;
INSERT INTO t1 VALUES (20);
--save_master_pos

--connection server_2
SET @saved_mode= @@GLOBAL.slave_ddl_exec_mode;
SET GLOBAL slave_ddl_exec_mode=STRICT;
SET sql_slave_skip_counter=1;
START SLAVE UNTIL master_gtid_pos="3-1-100";
--let $master_pos=3-1-100
--source include/sync_with_master_gtid.inc
--source include/wait_for_slave_to_stop.inc
--error ER_NO_SUCH_TABLE
SELECT * FROM t2;
SELECT IF(LOCATE("3-1-100", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 3-1-100 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

# Start the slave again, it should fail on the DROP TABLE as the table is not there.
SET sql_log_bin=0;
CALL mtr.add_suppression("Slave: Unknown table 'test\\.t2' Error_code: 1051");
SET sql_log_bin=1;
START SLAVE;
--let $slave_sql_errno=1051
--source include/wait_for_slave_sql_error.inc
SELECT IF(LOCATE("3-1-100", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 3-1-100 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

STOP SLAVE IO_THREAD;
SET sql_slave_skip_counter=2;
--source include/start_slave.inc
--sync_with_master

SELECT * FROM t1 WHERE a >= 20 ORDER BY a;
SELECT IF(LOCATE("3-1-101", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 3-1-101 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;
SELECT IF(LOCATE("2-1-1040", @@GLOBAL.gtid_slave_pos)>0, "Ok", CONCAT("ERROR! expected GTID 2-1-1040 not found in gtid_slave_pos: ", @@GLOBAL.gtid_slave_pos)) AS status;

SET GLOBAL slave_ddl_exec_mode= @saved_mode;


--echo *** Test GTID-connecting to a master with out-of-order sequence numbers in the binlog. ***

# Create an out-of-order binlog on server 2.
# Let server 3 replicate to an out-of-order point, stop it, restart it,
# and check that it replicates correctly despite the out-of-order.

--connection server_1
SET gtid_domain_id= @@GLOBAL.gtid_domain_id;
INSERT INTO t1 VALUES (31);
--save_master_pos

--connection server_2
--sync_with_master
SET gtid_domain_id= @@GLOBAL.gtid_domain_id;
INSERT INTO t1 VALUES (32);

--connection server_1
INSERT INTO t1 VALUES (33);
--save_master_pos

--connection server_2
--sync_with_master
--save_master_pos

--connection server_3
--sync_with_master
--source include/stop_slave.inc

--connection server_1
INSERT INTO t1 VALUES (34);
--save_master_pos

--connection server_2
--sync_with_master
--save_master_pos

--connection server_3
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 30 ORDER BY a;
--save_master_pos

--connection server_4
--sync_with_master
SELECT * FROM t1 WHERE a >= 30 ORDER BY a;


# Clean up.
--connection server_1
DROP TABLE t1;


--source include/rpl_end.inc
