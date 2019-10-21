--echo #
--echo # MDEV-17863 DROP TEMPORARY TABLE creates a transaction in
--echo # binary log on read only server
--echo # MDEV-19074 Improved read_only mode for slaves with
--echo # gtid_strict_mode enabled
--echo #

create user test@localhost;
grant CREATE, DROP, INSERT, SELECT on *.* to test@localhost;
create table t1 (a int) engine=myisam;
insert into t1 values (1),(2);
reset master;

set global read_only=1;
--echo # Ensure that optimize and analyze doesn't log to binary log
connect (con1,localhost,test,,test);
--error ER_OPTION_PREVENTS_STATEMENT
insert into t1 values(3);
analyze table t1;
check table t1;
repair table t1;
--error ER_OPTION_PREVENTS_STATEMENT
optimize table t1;

--echo # Ensure that using temporary tables is not logged
create temporary table tmp1 (a int) engine=myisam;
insert into tmp1 values (1),(2);
update tmp1 set a=10 where a=2;
delete from tmp1 where a=1;
create temporary table tmp2 select * from t1;
select * from tmp1;
select * from tmp2;
create temporary table tmp3 like t1;
create or replace temporary table tmp3 like t1;
alter table tmp2 add column (b int);
select * from tmp2;
--error ER_OPTION_PREVENTS_STATEMENT
insert into t1 select a+100 from tmp2;
drop table tmp1,tmp2,tmp3;

--echo # Clean up test connection
disconnect con1;
connection default;

--echo # Execute some commands as root that should not be logged
optimize table t1;
repair table t1;

--echo # Changes to temporary tables created under readonly should not
--echo # be logged
create temporary table tmp4 (a int) engine=myisam;
insert into tmp4 values (1),(2);
create temporary table tmp5 (a int) engine=myisam;
insert into tmp5 select * from tmp4;
alter table tmp5 add column (b int);

set global read_only=0;

insert into tmp4 values (3),(4);
insert into tmp5 values (10,3),(11,4);
select * from tmp4;
select * from tmp5;
update tmp4 set a=10 where a=2;
delete from tmp4 where a=1;
create table t2 select * from tmp4;
alter table tmp5 add column (c int);
insert into tmp5 values (20,5,1),(21,5,2);
select * from tmp5;
insert into t1 select a+200 from tmp5;
select * from t1;
drop table tmp4,tmp5;

--echo # Check what is logged. Only last create select and the insert...select's should be
--echo # row-logged
source include/show_binlog_events.inc;

--echo # Clean up
drop user test@localhost;
drop table t1,t2;
