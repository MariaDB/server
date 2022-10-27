--source include/have_rocksdb.inc
--source include/have_debug_sync.inc

--source include/count_sessions.inc

# This is a test case to reproduce https://github.com/facebook/mysql-5.6/issues/162
# Expected output of the last select for update was (1,2,100) and (1,3,100), but
# currently it returns (1,2,1) and (1,3,1), which must be fixed.

connect (con, localhost, root,,);
connection default;

set debug_sync='RESET';
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
create table t1 (id1 int, id2 int, value int, primary key (id1, id2), index sk (id1, value)) engine=rocksdb;
insert into t1 values (1, 1, 1),(1, 2, 1),(1, 3, 1),(1, 4, 1),(1, 5, 1),(2, 2, 2);

# deleting a first row
--echo --First row delete with $index
connection con;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
set debug_sync='rocksdb.get_row_by_rowid SIGNAL parked WAIT_FOR go';
send_eval update t1 force index ($index) set value=100 where id1=1;

connection default;
set debug_sync='now WAIT_FOR parked';
delete from t1 where id1=1 and id2=1;
set debug_sync='now SIGNAL go';

connection con;
reap;
select * from t1 where id1=1;

# deleting a middle row
--echo --Middle row delete with $index
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
set debug_sync='rocksdb.get_row_by_rowid SIGNAL parked WAIT_FOR go';
send_eval update t1 force index ($index) set value=200 where id1=1;

connection default;
set debug_sync='now WAIT_FOR parked';
delete from t1 where id1=1 and id2=3;
set debug_sync='now SIGNAL go';

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
select * from t1 where id1=1;

# deleting the end row
--echo --End row delete with $index
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
set debug_sync='rocksdb.get_row_by_rowid SIGNAL parked WAIT_FOR go';
send_eval update t1 force index ($index) set value=300 where id1=1;

connection default;
set debug_sync='now WAIT_FOR parked';
delete from t1 where id1=1 and id2=5;
set debug_sync='now SIGNAL go';

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
select * from t1 where id1=1;

# Cleanup
connection default;
disconnect con;
set debug_sync='RESET';
drop table t1;
--source include/wait_until_count_sessions.inc
