#
# One round of the const-table blob race.
#
# Expects t1 (the const table), t2 (the table it is joined against), a
# procedure churn_blob_memory() that rewrites t1's blob memory, and
# $reader_id / $writer_id.
#

--connection default
# --ps-protocol executes every complete SELECT twice and compares the two
# result sets.  GET_LOCK() is recursive, so a doubly executed acquisition
# takes the gate twice while the single RELEASE_LOCK() below drops only one
# reference.  The gate would then stay shut and the reader would sit there
# until its own timeout expired.
--disable_ps2_protocol # because SELECT with side effects
SELECT GET_LOCK('const_row_gate', 60);
--enable_ps2_protocol

--connection reader
# t1 holds a single row, so it is read during optimization and its row is
# kept in record[0] for the rest of the statement.  GET_LOCK() is evaluated
# per row of t2 during execution, i.e. after the const row is in hand and
# after the const tables have been unlocked, and parks the statement there.
--send SELECT LEFT(t1.b, 16) AS head, LENGTH(t1.b) AS len FROM t1, t2 WHERE t2.a = GET_LOCK('const_row_gate', 60)

--connection default
let $wait_condition=
  SELECT COUNT(*) FROM information_schema.PROCESSLIST
   WHERE ID = $reader_id AND STATE = 'User lock';
--source include/wait_condition.inc

--connection writer
--send CALL churn_blob_memory()

--connection default
# The procedure signals before its first statement, so once the signal
# arrives the writer is executing.  From there it either blocks on t1's
# surviving read lock or runs to completion; both mean it has had its turn
# and the reader can be let go.
SET DEBUG_SYNC= 'now WAIT_FOR writer_started';
let $wait_condition=
  SELECT COUNT(*) FROM information_schema.PROCESSLIST
   WHERE ID = $writer_id
     AND (STATE = 'Waiting for table level lock' OR COMMAND = 'Sleep');
--source include/wait_condition.inc

DO RELEASE_LOCK('const_row_gate');

--connection reader
--reap
# The reader took the gate over when it was released.
DO RELEASE_ALL_LOCKS();

--connection writer
--reap

--connection default
SET DEBUG_SYNC= 'RESET';
