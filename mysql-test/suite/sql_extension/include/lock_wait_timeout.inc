# SQL a handler runs waits for a row lock until innodb_lock_wait_timeout
# (1 second). Without innodb_rollback_on_timeout only that statement is
# rolled back; with it, the client's whole transaction. Takes:
#   $sqlext_lw_verb   the verb, run as TEST SQLEXT $sqlext_lw_verb <stmt>
#   $sqlext_lw_stmt   the statement; it waits for db1.t5 row 2
#   $sqlext_lw_in_trx 1 to run in BEGIN, after an INSERT into db1.t6; 0 to
#                     run in autocommit
#   $sqlext_lw_error  the error the statement must fail with, or 0
# Needs db1.t5 (id INT PRIMARY KEY, v INT) with rows 1 and 2, and db1.t6
# (a INT), both InnoDB. Runs on the default connection, leaves the caller
# there, and resets both tables.
connect (con_lw, localhost, root,,);
BEGIN;
UPDATE db1.t5 SET v= 2 WHERE id= 2;
connection default;
SET innodb_lock_wait_timeout= 1;
if ($sqlext_lw_in_trx)
{
  BEGIN;
  INSERT INTO db1.t6 VALUES (100);
}
--error $sqlext_lw_error
eval TEST SQLEXT $sqlext_lw_verb $sqlext_lw_stmt;
SET innodb_lock_wait_timeout= DEFAULT;
if ($sqlext_lw_in_trx)
{
  --echo # Whether the client's transaction, with its INSERT, is still there
  SELECT @@in_transaction;
  SELECT COUNT(*) AS n FROM db1.t6 WHERE a = 100;
  ROLLBACK;
}
connection con_lw;
ROLLBACK;
disconnect con_lw;
connection default;
UPDATE db1.t5 SET v= 0;
DELETE FROM db1.t6;
