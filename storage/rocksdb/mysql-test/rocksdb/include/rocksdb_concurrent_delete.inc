# Usage:
#
# let $order = ASC;  # or DESC
# let $comment = "rev:cf2";  # or ""
# --source suite/rocksdb/include/rocksdb_concurrent_delete.inc

let $first_row = -1; # Error this should never happen
if ($order == 'ASC')
{
  let $first_row = 1;
}
if ($order == 'DESC')
{
  let $first_row = 3;
}

connect (con, localhost, root,,);
connection default;

--disable_warnings
SET debug_sync='RESET';
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (pk INT PRIMARY KEY COMMENT $comment, a INT);
INSERT INTO t1 VALUES(1,1), (2,2), (3,3);

# This will cause the SELECT to block after finding the first row, but
# before locking and reading it.
connection con;
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

# Cleanup
connection default;
disconnect con;
set debug_sync='RESET';
drop table t1;
