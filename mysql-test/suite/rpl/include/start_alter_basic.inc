#
# Run start alter basic tests (CA/RA with given engine)
# Params:-
#  $engine
#  $sync_slave
#  master_node and slave_node connection should be defined

--echo # $engine
--connection master_node
--eval create table t1(a int, b int) engine=$engine;
insert into t1 values(1,1);
insert into t1 values(2,2);
--echo # Normal Alter
alter table t1 add column c int;
show create table t1;
--echo # Failed Alter
insert into t1 values(1,1, NULL);
--error ER_DUP_ENTRY
alter table t1 change a a int unique;
set @@session.binlog_alter_two_phase = 0;
alter table t1 change a a int;
set @@session.binlog_alter_two_phase = 1;
alter table t1 change a a int;
show create table t1;

# The following restriction should be removed post MDEV-26005 fix.
# TODO MDEV-26130 , should be removed after fixing
#if ($engine != 'aria')
#{
#--eval create temporary table tmp_tbl(a int, b int) engine=$engine;
#insert into tmp_tbl values(1,1);
#insert into tmp_tbl values(2,2);
#--echo # Normal Alter
#alter table tmp_tbl add column c int;
#--echo # Failed Alter
#insert into tmp_tbl values(1,1, NULL);
#--error ER_DUP_ENTRY
#alter table tmp_tbl change a a int unique ;
#show create table tmp_tbl;
#}
if ($sync_slave)
{
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  show create table t1;
}

--connection master_node
drop table t1;
#if ($engine != 'aria')
#{
#drop temporary table tmp_tbl;
#}
if ($sync_slave)
{
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
}
