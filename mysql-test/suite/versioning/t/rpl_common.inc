--source include/have_partition.inc

--echo #
--echo # MDEV-25347 DML events for auto-partitioned tables are written into binary log twice
--echo #
flush binary logs;
create table t1 (a int) with system versioning
partition by system_time limit 1 auto;

insert into t1 values (1);
update t1 set a= a + 1;
update t1 set a= a + 2;
--replace_result InnoDB ENGINE MyISAM ENGINE MEMORY ENGINE
show create table t1;
select * from t1;

--let $binlog_file=master-bin.000002
--source include/show_binlog_events.inc

--sync_slave_with_master
--replace_result InnoDB ENGINE MyISAM ENGINE MEMORY ENGINE
show create table t1;

select * from t1;
--connection master
# cleanup
drop table t1;

--source include/rpl_end.inc
