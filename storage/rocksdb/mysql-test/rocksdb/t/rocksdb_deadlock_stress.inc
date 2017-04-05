#
# Stress tests deadlock detection
#

--source include/have_rocksdb.inc

create table t1 (a int primary key, b int) engine=rocksdb;

set @prior_rocksdb_lock_wait_timeout = @@rocksdb_lock_wait_timeout;
set @prior_rocksdb_deadlock_detect = @@rocksdb_deadlock_detect;
set global rocksdb_lock_wait_timeout = 100000;
set global rocksdb_deadlock_detect = ON;

exec python ../storage/rocksdb/mysql-test/rocksdb/t/rocksdb_deadlock_stress.py root 127.0.0.1 $MASTER_MYPORT test t1 10000 10;

set global rocksdb_lock_wait_timeout = @prior_rocksdb_lock_wait_timeout;
set global rocksdb_deadlock_detect = @prior_rocksdb_deadlock_detect;
drop table t1;
