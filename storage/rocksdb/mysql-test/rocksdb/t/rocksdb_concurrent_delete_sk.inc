--source include/have_rocksdb.inc
--source include/have_debug_sync.inc

--source include/count_sessions.inc

connect (con, localhost, root,,);
connection default;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;

SET debug_sync='RESET';

eval CREATE TABLE t1 (pk INT PRIMARY KEY, a INT, index a(a));
INSERT INTO t1 VALUES(1,1), (2,2), (3,3), (4,4), (5,5);

# This will cause the SELECT to block after finding the first row, but
# before locking and reading it.
--echo --SK first row delete
connection con;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
SET debug_sync='rocksdb_concurrent_delete_sk SIGNAL parked WAIT_FOR go';
send_eval SELECT a FROM t1 FORCE INDEX(a) FOR UPDATE;

# While that connection is waiting, delete the first row (the one con
# is about to lock and read
connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = 1;

# Signal the waiting select to continue
SET debug_sync='now SIGNAL go';

connection con;
reap;

# Deleting a middle row
--echo --SK middle row delete
SET debug_sync='rocksdb_concurrent_delete_sk SIGNAL parked WAIT_FOR go';
send_eval SELECT a FROM t1 FORCE INDEX(a) FOR UPDATE;

connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = 3;
SET debug_sync='now SIGNAL go';

connection con;
if ($isolation_level == "REPEATABLE READ")
{
  --error ER_LOCK_DEADLOCK
  reap;
}
if ($isolation_level == "READ COMMITTED")
{
  reap;
}

# Deleting the end row
--echo --SK end row delete
SET debug_sync='rocksdb_concurrent_delete_sk SIGNAL parked WAIT_FOR go';
send_eval SELECT a FROM t1 FORCE INDEX(a) FOR UPDATE;

connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = 5;
SET debug_sync='now SIGNAL go';

connection con;
if ($isolation_level == "REPEATABLE READ")
{
  --error ER_LOCK_DEADLOCK
  reap;
}
if ($isolation_level == "READ COMMITTED")
{
  reap;
}

# Cleanup
connection default;
disconnect con;
set debug_sync='RESET';
drop table t1;
--source include/wait_until_count_sessions.inc
