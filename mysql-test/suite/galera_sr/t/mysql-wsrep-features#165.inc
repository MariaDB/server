# --source include/galera_cluster.inc
# --source include/have_debug_sync.inc

#
# This test attempts to catch a race condition between autocommit
# transaction and transaction which is rolling back due to
# deadlock.
#
# Test outline:
# * Trx 1a makes updates
# * SR trx 1b writes a row 3, then makes updates
# * AC trx 1c will attempt to write row 3 and will wait for lock
#   held by 1b
# * Sync point is set for 1b to delay SR rollback
# * SR trx 1b makes an update which makes it conflict with trx 1a
# * Slave shows BF - BF conflict and fails in applying write event

--connection node_1

CREATE TABLE t1 (f1 INTEGER PRIMARY KEY, f2 VARCHAR(1)) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1, 'x'), (2, 'x'), (4, 'x'), (5, 'x');

# --connect node_1a, 127.0.0.1, root, , test, $NODE_MYPORT_1
# --connect node_1b, 127.0.0.1, root, , test, $NODE_MYPORT_1
# --connect node_1c, 127.0.0.1, root, , test, $NODE_MYPORT_1

--connection node_1a
START TRANSACTION;

UPDATE t1 SET f2 = 'a' WHERE f1 = 1;
UPDATE t1 SET f2 = 'a' WHERE f1 = 4;
UPDATE t1 SET f2 = 'a' WHERE f1 = 5;


--connection node_1b
START TRANSACTION;
SET SESSION wsrep_trx_fragment_size = 1;
INSERT INTO t1 VALUES (3, 'b');
UPDATE t1 SET f2 = 'b' WHERE f1 = 2;

--connection node_2
SELECT * FROM t1;

# Will block, waiting for 1b
--connection node_1c
SET AUTOCOMMIT=ON;
--send INSERT INTO t1 VALUES (3, 'c')

--connection node_2
SELECT * FROM t1;

# Will block, waiting for 1b
--connection node_1a
--send UPDATE t1 SET f2 = 'a' WHERE f1 = 2

--connection node_1
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE USER != 'system user' AND STATE = 'Updating';
--source include/wait_condition.inc

# Will deadlock
--connection node_1b
SET DEBUG_SYNC = 'wsrep_before_SR_rollback SIGNAL wait WAIT_FOR continue';
--send UPDATE t1 SET f2 = 'b' WHERE f1 = 1

# Wait until 1b hits rollback
--connection node_1
SET DEBUG_SYNC = 'now WAIT_FOR wait';

# UPDATE 12.06.2016: as of recent wsrep API changes, rollbacking thread no
# longer queues ROLLBACKs and blocks on ROLLBACK replication before performing
# the actual rollback. As a result this test is moot as both node_1a and node_1c
# connections are hanging now until sync point is released. Thus sync point
# release had to be moved above to release the connections. However it is not
# impossible that further changes in the code may reintroduce the race, so
# leaving the test as close to original as possible.
#
# --connection node_1a
# --reap
# COMMIT;
#
# --connection node_1c
# --reap
#
# UPDATE t1 SET f2 = 'x' WHERE f1 = 3;

--connection node_1
SET DEBUG_SYNC = 'now SIGNAL continue';

--connection node_1c
--reap

UPDATE t1 SET f2 = 'x' WHERE f1 = 3;

--connection node_1a
--reap
COMMIT;

--connection node_1b
--error ER_LOCK_DEADLOCK
--reap

--connection node_1
SELECT * FROM t1;
--connection node_2
SELECT * FROM t1;

--connection node_1
SET DEBUG_SYNC = 'RESET';
DROP TABLE t1;
