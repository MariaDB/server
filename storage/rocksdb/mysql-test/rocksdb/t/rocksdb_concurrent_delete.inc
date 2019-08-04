# MariaDB: including the below too many times causes really long argv list
# in win_main()'s argument which blows up some limit on Windows.
# Comment it out:
#--source include/have_rocksdb.inc
--source include/have_debug_sync.inc

--source include/count_sessions.inc

# Usage:
#
# let $order = ASC;  # or DESC
# let $comment = "rev:cf2";  # or ""
# --source suite/rocksdb/t/rocksdb_concurrent_delete.inc

let $first_row = -1; # Error this should never happen
if ($order == 'ASC')
{
  let $first_row = 1;
  let $middle_row = 3;
  let $end_row = 5;
}
if ($order == 'DESC')
{
  let $first_row = 5;
  let $middle_row = 3;
  let $end_row = 1;
}

connect (con, localhost, root,,);
connection default;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;

SET debug_sync='RESET';

eval CREATE TABLE t1 (pk INT PRIMARY KEY COMMENT $comment, a INT);
INSERT INTO t1 VALUES(1,1), (2,2), (3,3), (4,4), (5,5);

# This will cause the SELECT to block after finding the first row, but
# before locking and reading it.
--echo --PK first row delete
connection con;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
SET debug_sync='rocksdb_concurrent_delete SIGNAL parked WAIT_FOR go';
send_eval SELECT * FROM t1 order by t1.pk $order FOR UPDATE;

# While that connection is waiting, delete the first row (the one con
# is about to lock and read
connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = $first_row;

# Signal the waiting select to continue
SET debug_sync='now SIGNAL go';

# Now get the results from the select.  The first entry (1,1) (or (3,3) when
# using reverse ordering) should be missing.  Prior to the fix the SELECT
# would have returned: "1815: Internal error: NotFound:"
connection con;
reap;

# Deleting a middle row
--echo --PK middle row delete
SET debug_sync='rocksdb_concurrent_delete SIGNAL parked WAIT_FOR go';
send_eval SELECT * FROM t1 order by t1.pk $order FOR UPDATE;

connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = $middle_row;
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
--echo --PK end row delete
SET debug_sync='rocksdb_concurrent_delete SIGNAL parked WAIT_FOR go';
send_eval SELECT * FROM t1 order by t1.pk $order FOR UPDATE;

connection default;
SET debug_sync='now WAIT_FOR parked';
eval DELETE FROM t1 WHERE pk = $end_row;
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
