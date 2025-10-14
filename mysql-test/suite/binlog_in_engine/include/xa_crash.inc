--source include/reset_master.inc

--echo *** Test XA PREPARE, COMMIT, ROLLBACK with crashes at various points.
eval CREATE TABLE t1(a INT NOT NULL, b INT NOT NULL, c LONGBLOB, PRIMARY KEY(a, b)) ENGINE=$engine;

INSERT INTO t1 VALUES (0, 1, '');
INSERT INTO t1 VALUES (0, 2, '');
INSERT INTO t1 VALUES (0, 3, '');

--echo *** x1: prepared in binlog but not in engine/tables.
--connect con1,localhost,root,,
XA BEGIN 'x1';
INSERT INTO t1 VALUES (1, 1, '');
INSERT INTO t1 VALUES (1, 2, REPEAT('<x1>', 10000));

--echo *** x2: prepared in binlog and engine.
--connect con2,localhost,root,,
XA BEGIN 'x2';
INSERT INTO t1 VALUES (2, 1, REPEAT('<x2>', 10000));

--echo *** x3: committed in binlog, not in engine.
--connect con3,localhost,root,,
XA BEGIN 'x3';
INSERT INTO t1 VALUES (3, 1, '');

--connection default
FLUSH BINARY LOGS;

--echo *** x4: committed in binlog and in engine.
--connect con4,localhost,root,,
XA BEGIN 'x4';
INSERT INTO t1 VALUES (4, 1, REPEAT('<x4>', 10000));

--echo *** x5: rolled back in binlog, not in engine.
--connect con5,localhost,root,,
XA BEGIN 'x5';
INSERT INTO t1 VALUES (5, 1, '');

--echo *** x6: rolled back in binlog and in engine.
--connect con6,localhost,root,,
XA BEGIN 'x6';
INSERT INTO t1 VALUES (6, 1, REPEAT('<x4>', 10000));

--connection default
FLUSH BINARY LOGS;

--connection con3
INSERT INTO t1 VALUES (3, 2, '');
XA END 'x3';
XA PREPARE 'x3';
SET debug_sync= 'ibb_after_commit_redo_log SIGNAL con3_rdy WAIT_FOR crash';
send XA COMMIT 'x3';

--connection default
SET debug_sync= 'now WAIT_FOR con3_rdy';

--connection con1
INSERT INTO t1 VALUES (1, 3, '');
XA END 'x1';
SET debug_sync= 'ibb_after_prepare_redo_log SIGNAL con1_rdy WAIT_FOR crash';
send XA PREPARE 'x1';

--connection default
SET debug_sync= 'now WAIT_FOR con1_rdy';

--connection con6
INSERT INTO t1 VALUES (6, 2, '');
XA END 'x6';
XA PREPARE 'x6';
SET debug_sync= 'rollback_handlerton_after SIGNAL con6_rdy WAIT_FOR crash';
send XA ROLLBACK 'x6';

--connection default
SET debug_sync= 'now WAIT_FOR con6_rdy';

--connection con5
INSERT INTO t1 VALUES (5, 2, '');
XA END 'x5';
XA PREPARE 'x5';
SET debug_sync= 'ibb_after_rollback_redo_log SIGNAL con5_rdy WAIT_FOR crash';
send XA ROLLBACK 'x5';

--connection default
SET debug_sync= 'now WAIT_FOR con5_rdy';

FLUSH BINARY LOGS;

--connection con2
INSERT INTO t1 VALUES (2, 2, '');
INSERT INTO t1 VALUES (2, 3, REPEAT('<x2>', 10001));
XA END 'x2';
SET debug_sync= 'at_unlog_xa_prepare SIGNAL con2_rdy WAIT_FOR crash';
send XA PREPARE 'x2';

--connection default
SET debug_sync= 'now WAIT_FOR con2_rdy';

--connection con4
INSERT INTO t1 VALUES (4, 2, '');
XA END 'x4';
XA PREPARE 'x4';
SET debug_sync= 'commit_handlerton_after SIGNAL con4_rdy WAIT_FOR crash';
send XA COMMIT 'x4';

--connection default
SET debug_sync= 'now WAIT_FOR con4_rdy';

--let $shutdown_timeout=0
--source include/restart_mysqld.inc

--disconnect con1
--disconnect con2
--disconnect con3
--disconnect con4
--disconnect con5
--disconnect con6

--connection default
--sorted_result
XA RECOVER;

XA COMMIT 'x2';
--let $binlog_file= binlog-000000.ibb
--let $binlog_start= 4
--source include/show_binlog_events.inc
--let $binlog_file= binlog-000001.ibb
--source include/show_binlog_events.inc
--let $binlog_file= binlog-000002.ibb
--source include/show_binlog_events.inc
FLUSH BINARY LOGS;
--let $binlog_file= binlog-000003.ibb
--source include/show_binlog_events.inc

SELECT a, b, LENGTH(c) FROM t1 ORDER BY a, b;

--echo *** Test that we can purge, no leaked XA/OOB refs.
SET @old_min_slaves= @@GLOBAL.slave_connections_needed_for_purge;
SET GLOBAL slave_connections_needed_for_purge= 0;
FLUSH BINARY LOGS;
PURGE BINARY LOGS TO 'binlog-000004.ibb';
SET GLOBAL slave_connections_needed_for_purge= @old_min_slaves;

DROP TABLE t1;
