#
# Run $ddl on the FK parent table pdb.parent on node_1 while node_2 holds
# the applier of $ddl before execution, then replicate a DML on the FK
# child test.child that does not touch the FK column. The DML must depend
# on the DDL.
#
# $fk_checks_off  run $ddl with foreign_key_checks = 0
# $not_toi        $ddl is replicated as a transaction, not in TOI
#
--connection node_1
CREATE DATABASE pdb;
CREATE TABLE pdb.parent (id INT PRIMARY KEY, f CHAR(1), KEY k (f)) ENGINE=InnoDB;
CREATE TABLE pdb.tmpl LIKE pdb.parent;
CREATE TABLE child (id INT PRIMARY KEY, parent_id INT, cnt INT NOT NULL DEFAULT 0,
  FOREIGN KEY (parent_id) REFERENCES pdb.parent(id)) ENGINE=InnoDB;
INSERT INTO pdb.parent VALUES (1, 'a');
INSERT INTO child VALUES (1, 1, 0);

--connection node_2
--let $wait_condition = SELECT COUNT(*) = 1 FROM child
--source include/wait_condition.inc
--let $sync_point = wsrep_apply_toi
--let $sync_point_reached = sync.wsrep_apply_toi_reached
if ($not_toi)
{
  --let $sync_point = wsrep_apply_cb
  --let $sync_point_reached = sync.wsrep_apply_cb_reached
}
--eval SET GLOBAL DEBUG_DBUG = "d,sync.$sync_point"

--connection node_1
if ($fk_checks_off)
{
  SET SESSION foreign_key_checks = 0;
}
--eval $ddl
SET SESSION foreign_key_checks = DEFAULT;

--connection node_2
--eval SET DEBUG_SYNC = "now WAIT_FOR $sync_point_reached"
SET GLOBAL DEBUG_DBUG = '';
--let $expected_apply_waits = `SELECT VARIABLE_VALUE+1 FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_apply_waits'`

--connection node_1
UPDATE child SET cnt = cnt + 1 WHERE id = 1;

# The UPDATE must wait for the DDL to be applied. If it was applied in
# parallel, it would hold an IX lock on child waiting for the commit
# order, and the DDL would wait for an X lock on child forever.
--connection node_2
--let $wait_condition = SELECT VARIABLE_VALUE = $expected_apply_waits FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_apply_waits'
--source include/wait_condition.inc
if (!$success)
{
  --die UPDATE on the FK child was applied in parallel with the DDL
}
--eval SET DEBUG_SYNC = "now SIGNAL signal.$sync_point"
SET SESSION wsrep_sync_wait = DEFAULT;
SELECT cnt FROM child;
SET SESSION wsrep_sync_wait = 0;
SET DEBUG_SYNC = "RESET";

--connection node_1
DROP TABLE child;
DROP DATABASE IF EXISTS pdb;
--let $fk_checks_off = 0
--let $not_toi = 0
