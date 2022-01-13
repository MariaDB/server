# This test attempts to show that OPTIMIZE on a child table does NOT
# acquire MDL locks on the parent table. #
# param: $table_admin_command
#        DDL table command to test, script will build full SQL statement:
#        $table_admin_command TABLE c;
#
# param: $FK_constraint
#        Foreign key constraint to use when creating the child table.
#

CREATE TABLE parent (pk INTEGER PRIMARY KEY);
--eval CREATE TABLE child (pk INTEGER PRIMARY KEY, parent_id INTEGER, FOREIGN KEY(parent_id) REFERENCES parent(pk) $fk_constraint)

INSERT INTO parent VALUES (1), (2), (3), (4);
INSERT INTO child VALUES (1,1), (2,2), (3,3), (4,4);

--connection node_1
# Start a transaction that uses the parent table,
# so that we acquire MDL lock on parent.
START TRANSACTION;
SELECT * FROM parent FOR UPDATE;

# In a different connection, execute the table
# admin command (OPTIMIZE / REPAIR ...) on the child table.
--connect node_1a, 127.0.0.1, root, , test, $NODE_MYPORT_1
--eval $table_admin_command TABLE child;

# Expect no conflict.
--connection node_1
COMMIT;

DROP TABLE child, parent;

--disconnect node_1a
