# Testing of atomic CREATE TABLE with binary logging and side effects

--source include/have_innodb.inc
--source include/have_log_bin.inc
--source include/master-slave.inc

create table t10 (p int primary key, new int) engine=innodb;
insert into t10 values (1,1),(2,2),(3,1);
create table t11 (new int) engine=myisam;

delimiter |;
create function func_with_sideeffect(arg int)
returns integer
begin
  insert into t11 set new=arg;
  return arg+1;
end |
delimiter ;|

set @@global.binlog_annotate_row_events=1;

# Reset logs for easy tracking of changes
--disable_query_log
sync_slave_with_master;
connection slave;
stop slave;
connection master;
reset master;
connection slave;
start slave;
connection master;
--enable_query_log

--echo # Tables innodb, innodb, myisam
--echo # Should be logged

# Test for the that insert_select works properly
CREATE TABLE t1 (new int primary key, dup int) engine=innodb;
--error ER_DUP_ENTRY
insert into t1 select new,func_with_sideeffect(p) as 'dup' from t10;
drop table t1;

--error ER_DUP_ENTRY
CREATE TABLE t1 (new int primary key) engine=innodb select new,func_with_sideeffect(p) from t10;

--echo # Tables myisam, innodb, myisam
--echo # Should be logged
--error ER_DUP_ENTRY
CREATE TABLE t1 (new int primary key) engine=myisam select new,func_with_sideeffect(p+10) from t10;

alter table t11 engine=innodb;

--echo # Tables myisam, innodb, innodb
--echo # Should not be logged
--error ER_DUP_ENTRY
CREATE TABLE t1 (new int primary key) engine=myisam select new,func_with_sideeffect(p+20) from t10;

alter table t10 engine=myisam;

--echo # Tables innodb, innodb, innodb
--echo # Should not be logged
--error ER_DUP_ENTRY
CREATE TABLE t1 (new int primary key) engine=innodb select new,func_with_sideeffect(p+30) from t10;

--echo # Tables innodb, myisam, innodb
--echo # Should not be logged
--error ER_DUP_ENTRY
CREATE TABLE t1 (new int primary key) engine=innodb select new,func_with_sideeffect(p+40) from t10;

--source include/show_binlog_events2.inc

drop table t10,t11;
drop function func_with_sideeffect;
set @@global.binlog_annotate_row_events=default;

# test that binlog replicates correctly to slave
--sync_slave_with_master

--echo End of the tests
--source include/rpl_end.inc
