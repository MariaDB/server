--source include/count_sessions.inc

connect(stcon1,localhost,root,,test);
connect(stcon2,localhost,root,,test);

connection stcon1;

--disable_warnings
drop table if exists t1,t2,t3;
drop database if exists tempdb;
create database if not exists tempdb;
--enable_warnings

use test;
eval create temporary table t1 (t int) ENGINE = $engine;
eval create temporary table t2 (t int) ENGINE = $engine;
eval create temporary table tempdb.t3(t int) ENGINE=$engine;
eval create temporary table tempdb.t4(t int) ENGINE=$engine;

show create table t1;
insert into t1 values (1),(1),(1),(1);
insert into t2 values (2),(2),(2);
insert into tempdb.t3 values (3),(3);

# Show global temporary tables
--sorted_result
select * from information_schema.global_temporary_tables;
--sorted_result
select * from information_schema.global_temporary_tables where table_schema="tempdb";
--sorted_result
select * from information_schema.global_temporary_tables where table_schema="test";

connection stcon2;

--disable_warnings
drop table if exists tempdb.t2;
--enable_warnings
eval create temporary table t1(t int) ENGINE=$engine;
eval create temporary table tempdb.t2(t int) ENGINE=$engine;
show create table t1;

insert into t1 values (1);
insert into tempdb.t2 values (2),(2),(2),(2),(2);
--sorted_result
select * from information_schema.global_temporary_tables;
drop table tempdb.t2;
drop table t1;

disconnect stcon2;

connection stcon1;
--sorted_result
select * from information_schema.global_temporary_tables;

drop table t1,t2;
drop table tempdb.t3;
drop database tempdb;

connection default;
disconnect stcon1;

# Wait till all disconnects are completed
--source include/wait_until_count_sessions.inc
