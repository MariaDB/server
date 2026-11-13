#
# Commit a DML on the FK child on node_1 while $ddl on the FK parent is
# executing in TOI on node_1 (after replication, before the InnoDB commit
# that X-locks the child). The DML must fail certification: once it is
# ordered after the DDL it cannot be BF aborted anymore, and the DDL
# would wait for its IX lock on the child forever.
#
--connection node_1
CREATE TABLE parent (id INT PRIMARY KEY, f CHAR(1), KEY k (f)) ENGINE=InnoDB;
CREATE TABLE child (id INT PRIMARY KEY, parent_id INT, cnt INT NOT NULL DEFAULT 0,
  FOREIGN KEY (parent_id) REFERENCES parent(id)) ENGINE=InnoDB;
INSERT INTO parent VALUES (1, 'a');
INSERT INTO child VALUES (1, 1, 0);

--let $expected_cert_failures = `SELECT VARIABLE_VALUE+1 FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'`

--connection node_1a
BEGIN;
SELECT cnt FROM child WHERE id = 1;

--connection node_1
SET DEBUG_SYNC = "alter_table_inplace_before_commit SIGNAL ddl_reached WAIT_FOR ddl_continue";
--send_eval $ddl

--connection node_1a
SET DEBUG_SYNC = "now WAIT_FOR ddl_reached";
UPDATE child SET cnt = cnt + 1 WHERE id = 1;
--send COMMIT

--connection node_1b
--let $wait_condition = SELECT VARIABLE_VALUE = $expected_cert_failures FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'
--source include/wait_condition.inc
if (!$success)
{
  --die COMMIT of the DML on the FK child passed certification
}
SET DEBUG_SYNC = "now SIGNAL ddl_continue";

--connection node_1a
--error ER_LOCK_DEADLOCK
--reap

--connection node_1
--reap
SET DEBUG_SYNC = "RESET";
SELECT cnt FROM child;
DROP TABLE child, parent;
