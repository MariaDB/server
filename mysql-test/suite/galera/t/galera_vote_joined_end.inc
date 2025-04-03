# Confirm node #4 has rejoined
--connection node_1
--let $members = 4
--source include/wsrep_wait_membership.inc
#DROP TABLE IF EXISTS t2;

# Confirm that all is good and all nodes have identical data

--connection node_1
SELECT count(*) AS expect1_390 FROM t1;

#CALL mtr.add_suppression("Replica SQL: Could not execute Delete_rows");
#CALL mtr.add_suppression("Event 3 Delete_rows apply failed: 120, seqno [0-9]+");

--connection node_2
SELECT count(*) AS expect2_390 FROM t1;

#CALL mtr.add_suppression("mysqld: Can't find record in 't1'");
#CALL mtr.add_suppression("Replica SQL: Could not execute Delete_rows");
#CALL mtr.add_suppression("Event 3 Delete_rows apply failed: 120, seqno seqno [0-9]+");

--connection node_3
SELECT count(*) AS expect3_390 FROM t1;

--connection node_4
SELECT count(*) AS expect4_390 FROM t1;

DROP TABLE t1;
DROP PROCEDURE p1;

#CALL mtr.add_suppression("inconsistent with group");

--source ../include/auto_increment_offset_restore.inc
