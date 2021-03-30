# This file is used for common DDL log test for both partiotioned
# and non-partitioned tables.
# Parameters:
# $part_int  - partition definition for int field, must be empty for
#              non-partitioned testing
# $part_date - partition definition for date field, must be empty for
#              non-partitioned testing

--echo #
--echo # Testing with normal tables
--echo #

eval create table t1 (a int) engine=myisam $part_int;
insert into t1 values (1),(2);
alter table t1 add column b int;
alter table t1 rename as t2;
rename table t2 to t1;
truncate table t1;
repair table t1;
optimize table t1;
drop table t1;

eval create table t1_innodb (a int) engine=innodb $part_int;
insert into t1_innodb values (1),(2);
alter table t1_innodb add column b int;
alter table t1_innodb rename as t2_innodb;
rename table t2_innodb to t1_innodb;
truncate table t1_innodb;
repair table t1_innodb;
optimize table t1_innodb;
drop table t1_innodb;

--echo #
--echo # Testing with temporary tables (should not be logged)
--echo #

create temporary table tmp_t10 (a int) engine=myisam;
alter table tmp_t10 add column b int;
alter table tmp_t10 rename as tmp_t11;
rename table tmp_t11 to tmp_t10;
truncate table tmp_t10;
drop table tmp_t10;

--echo #
--echo # Testing with mix of normal and temporary tables
--echo #

create temporary table tmp_t20 (a int);
eval create table t20 (a int) $part_int;
drop table tmp_t20,t20;

create temporary table tmp_t21 (a int);
eval create table t21 (a int) $part_int;
drop temporary table if exists tmp_t21,t21;
drop table if exists tmp_t21,t21;

--echo #
--echo # Testing create select
--echo #

eval create table t30 (a int) $part_int;
insert into t30 values (1),(1);
eval create table t31 (a int primary key) $part_int select * from t30 limit 1;
create or replace table t31 select * from t30 limit 1;
create or replace temporary table t30_dup select * from t30 limit 1;
--error ER_DUP_ENTRY
eval create or replace table t31 (a int primary key) $part_int select * from t30;
eval create table t32 (a int) $part_int;
drop table if exists t30,t31,t32,tmp_t30;

--echo #
--echo # Testing create LIKE
--echo #
eval create table t40 (a int) engine=myisam $part_int;
eval create table t41 (a int, b int) engine=innodb $part_int;
create table t42 like t40;
if (!$part_int) {
create temporary table t43_tmp like t40;
}
create or replace table t42 like t41;
show create table t42;
drop table t40, t41, t42;

--echo #
--echo # Testing rename
--echo #

eval create table t50 (a int) $part_int;
eval create table t51 (a int, b int) $part_int;
rename table t50 to t52, t51 to t53;
rename table t52 to tmp, t53 to t52, tmp to t53;
drop table t52,t53;

--echo #
--echo # Testing enable/disable keys
--echo #

eval CREATE TABLE t60 (a int(10), index(a) ) ENGINE=Aria $part_int;
INSERT INTO t60 VALUES(1),(2),(3);
ALTER TABLE t60 DISABLE KEYS;
INSERT INTO t60 VALUES(4),(5),(6);
ALTER TABLE t60 ENABLE KEYS;
DROP TABLE t60;

CREATE TEMPORARY TABLE t61 (i int(10), index(i) ) ENGINE=Aria;
INSERT INTO t61 VALUES(1),(2),(3);
ALTER TABLE t61 DISABLE KEYS;
DROP TABLE t61;

--echo #
--echo # Testing load data
--echo #

eval create table t70 (a date, b date, c date not null, d date) engine=aria $part_date;
--disable_warnings
load data infile '../../std_data/loaddata1.dat' ignore into table t70 fields terminated by ',';
load data infile '../../std_data/loaddata1.dat' ignore into table t70 fields terminated by ',';
truncate table t70;
lock table t70 write;
load data infile '../../std_data/loaddata1.dat' ignore into table t70 fields terminated by ',';
load data infile '../../std_data/loaddata1.dat' ignore into table t70 fields terminated by ',';
unlock tables;
--enable_warnings

eval create table t71 (a date, b date, c date not null, d date) engine=aria $part_date;
lock tables t71 write, t70 read;
insert into t71 select * from t70;
unlock tables;
drop table t70,t71;

--echo #
--echo # Testing strange table names
--echo #

eval create table `t 1` (a int) $part_int;
drop table `t 1`;

--echo #
--echo # Testing views and triggers
--echo #

eval create table t80 (a int, b int) engine=myisam $part_int;
create view v1 as select * from t80;
create trigger trg before insert on t80 for each row set @b:=1;
drop trigger trg;
drop view v1;
drop table t80;

--echo #
--echo # Testing alter to a new storage engine
--echo #

eval create table t85 (a int primary key, b int) engine=myisam $part_int;
alter table t85 engine=innodb;
drop table t85;
