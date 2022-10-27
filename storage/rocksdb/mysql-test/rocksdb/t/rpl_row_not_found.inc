--source include/have_rocksdb.inc
--source include/master-slave.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc

connection master;
--disable_warnings
drop table if exists t1;
--enable_warnings

connection master;

create table t0 (a int) engine=myisam;
insert into t0 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);
create table t1(a int) engine=myisam;
insert into t1 select A.a + B.a* 10 + C.a * 100 from t0 A, t0 B, t0 C;
create table t2 (
  pk int primary key,
  kp1 int,
  kp2 int,
  col1 int,
  key (kp1,kp2)
) engine=rocksdb;
# Use RBR for next few statements to avoid the 
#  'Unsafe statement written to the binary log' warnings.
set @tmp_binlog_format=@@binlog_format;
set @@binlog_format=ROW;
insert into t2 select a,a,a,a from t1;
create table t3 like t2;
insert into t3 select * from t2;
set binlog_format=@tmp_binlog_format;


# For GitHub issue#166
# Slave is suspended at ha_rocksdb::read_range_first() -> index_read_map_impl()
# -> ha_rocksdb::get_row_by_rowid() -- which is after creating an iterator,
# Seek(), Next() (getting pk=1)
# and before GetForUpdate() and before creating a snapshot.
# Deletes remove pk=2 and pk=3, then resumes update on slave.
# The update resumes with GetForUpdate(pk=1),
# index_next() -> secondary_index_read() -> get_row_by_rowid(pk=2)
# then doesn't find a row.
# The slave should not stop with error (Can't find a record).

--source include/sync_slave_sql_with_master.inc

connection slave;
let $old_debug = `select @@global.debug`;
set global debug_dbug= 'd,dbug.rocksdb.get_row_by_rowid';
--source include/stop_slave.inc
--source include/start_slave.inc

connection master;
update t2 set col1=100 where kp1 between 1 and 3 and mod(kp2,2)=0;

connection slave;
set debug_sync= 'now WAIT_FOR Reached';
eval set global debug_dbug = '$old_debug';
set sql_log_bin=0;
delete from t2 where pk=2;
delete from t2 where pk=3;
set debug_sync= 'now SIGNAL signal.rocksdb.get_row_by_rowid_let_running';

connection master;
--source include/sync_slave_sql_with_master.inc
connection slave;
select * from t2 where pk < 5;

# For GitHub issue#162 (result file must be updated after fixing #162)
connection slave;
set global debug_dbug= 'd,dbug.rocksdb.get_row_by_rowid';
--source include/stop_slave.inc
--source include/start_slave.inc

connection master;
update t3 set col1=100 where kp1 between 1 and 4 and mod(kp2,2)=0;

connection slave;
call mtr.add_suppression("Deadlock found when trying to get lock");
set debug_sync= 'now WAIT_FOR Reached';
eval set global debug_dbug = '$old_debug';
set sql_log_bin=0;
delete from t3 where pk=2;
delete from t3 where pk=3;
set debug_sync= 'now SIGNAL signal.rocksdb.get_row_by_rowid_let_running';

connection master;
--source include/sync_slave_sql_with_master.inc
connection slave;
# col1 for pk=4 should be 100
select * from t3 where pk < 5;

set debug_sync='RESET';
# Cleanup
connection master;
drop table t0, t1, t2, t3;
--source include/rpl_end.inc

