--echo Performing State Transfer on a server that has been temporarily disconnected

--connection node_1
CREATE TABLE t1 (id int not null primary key,f1 CHAR(255)) ENGINE=InnoDB;
SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (1,'node1_committed_before');
INSERT INTO t1 VALUES (2,'node1_committed_before');
INSERT INTO t1 VALUES (3,'node1_committed_before');
INSERT INTO t1 VALUES (4,'node1_committed_before');
INSERT INTO t1 VALUES (5,'node1_committed_before');
COMMIT;

--connection node_2
SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (6,'node2_committed_before');
INSERT INTO t1 VALUES (7,'node2_committed_before');
INSERT INTO t1 VALUES (8,'node2_committed_before');
INSERT INTO t1 VALUES (9,'node2_committed_before');
INSERT INTO t1 VALUES (10,'node2_committed_before');
COMMIT;

--source suite/galera/include/galera_stop_replication.inc

--connection node_1
--let $wait_condition = SELECT VARIABLE_VALUE = 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc

SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (11,'node1_committed_during');
INSERT INTO t1 VALUES (12,'node1_committed_during');
INSERT INTO t1 VALUES (13,'node1_committed_during');
INSERT INTO t1 VALUES (14,'node1_committed_during');
INSERT INTO t1 VALUES (15,'node1_committed_during');
COMMIT;

START TRANSACTION;
INSERT INTO t1 VALUES (16,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (17,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (18,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (19,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (20,'node1_to_be_committed_after');

--connect node_1a_galera_st_disconnect_slave, 127.0.0.1, root, , test, $NODE_MYPORT_1
SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (21,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (22,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (23,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (24,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (25,'node1_to_be_rollbacked_after');

--connection node_2
--source suite/galera/include/galera_start_replication.inc

#
# client connections were killed by provider load, so have to re-open here
#
--disconnect node_2
--connect node_2, 127.0.0.1, root, , test, $NODE_MYPORT_2
--connection node_2
--enable_reconnect

--let $wait_condition = SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc

SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (26,'node2_committed_after');
INSERT INTO t1 VALUES (27,'node2_committed_after');
INSERT INTO t1 VALUES (28,'node2_committed_after');
INSERT INTO t1 VALUES (29,'node2_committed_after');
INSERT INTO t1 VALUES (30,'node2_committed_after');
COMMIT;

--connection node_1
INSERT INTO t1 VALUES (31,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (32,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (33,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (34,'node1_to_be_committed_after');
INSERT INTO t1 VALUES (35,'node1_to_be_committed_after');
COMMIT;

SET AUTOCOMMIT=OFF;
START TRANSACTION;
INSERT INTO t1 VALUES (36,'node1_committed_after');
INSERT INTO t1 VALUES (37,'node1_committed_after');
INSERT INTO t1 VALUES (38,'node1_committed_after');
INSERT INTO t1 VALUES (39,'node1_committed_after');
INSERT INTO t1 VALUES (40,'node1_committed_after');
COMMIT;

--connection node_1a_galera_st_disconnect_slave
INSERT INTO t1 VALUES (41,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (42,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (43,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (44,'node1_to_be_rollbacked_after');
INSERT INTO t1 VALUES (45,'node1_to_be_rollbacked_after');
ROLLBACK;

--let $wait_condition = SELECT COUNT(*)=35 FROM t1
--source include/wait_condition.inc

SET AUTOCOMMIT=ON;
SET SESSION wsrep_sync_wait=15;
SELECT COUNT(*) AS EXPECT_35 FROM t1;
SELECT * FROM t1;
SELECT COUNT(*) = 0 FROM (SELECT COUNT(*) AS c, f1 FROM t1 GROUP BY f1 HAVING c NOT IN (5, 10)) AS a1;

--connection node_1
--let $wait_condition = SELECT COUNT(*)=35 FROM t1
--source include/wait_condition.inc

SET AUTOCOMMIT=ON;
SET SESSION wsrep_sync_wait=15;
SELECT COUNT(*) AS EXPECT_35 FROM t1;
SELECT * FROM t1;
SELECT COUNT(*) = 0 FROM (SELECT COUNT(*) AS c, f1 FROM t1 GROUP BY f1 HAVING c NOT IN (5, 10)) AS a1;
DROP TABLE t1;
COMMIT;
