# SQL a handler runs is chosen as the victim of an InnoDB deadlock, which
# rolls back the client's transaction. Takes:
#   $sqlext_dl_verb   the verb, run as TEST SQLEXT $sqlext_dl_verb <stmt>
#   $sqlext_dl_stmt   the statement; it updates db1.t5 row 2 (row 1 too
#                     in autocommit), where the other connection then
#                     waits for row 1
#   $sqlext_dl_in_trx 1 to run in BEGIN, after an INSERT into db1.t6 and
#                     an update of db1.t5 row 1; 0 to run in autocommit
#   $sqlext_dl_error  the error the statement must fail with, or 0
# Needs db1.t5 (id INT PRIMARY KEY, v INT) with rows 1 and 2, and db1.t6
# (a INT), both InnoDB. Runs on the default connection, leaves the caller
# there, and resets both tables.
connect (con_dl, localhost, root,,);
--echo # The other transaction does more work, so InnoDB picks the client's
--echo # as the victim
BEGIN;
INSERT INTO db1.t6 VALUES (1), (2), (3), (4), (5), (6), (7), (8);
UPDATE db1.t5 SET v= 2 WHERE id= 2;
connection default;
if ($sqlext_dl_in_trx)
{
  BEGIN;
  INSERT INTO db1.t6 VALUES (100);
  UPDATE db1.t5 SET v= 1 WHERE id= 1;
}
send_eval TEST SQLEXT $sqlext_dl_verb $sqlext_dl_stmt;
connection con_dl;
let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.innodb_trx
  WHERE trx_state = 'LOCK WAIT';
--source include/wait_condition.inc
UPDATE db1.t5 SET v= 2 WHERE id= 1;
COMMIT;
disconnect con_dl;
connection default;
--error $sqlext_dl_error
reap;
if ($sqlext_dl_in_trx)
{
  --echo # The client's transaction is gone, with its INSERT
  SELECT @@in_transaction;
  SELECT COUNT(*) AS n FROM db1.t6 WHERE a = 100;
}
SELECT * FROM db1.t5 ORDER BY id;
UPDATE db1.t5 SET v= 0;
DELETE FROM db1.t6;
