--source include/innodb_page_size.inc

#
# MMDEV-11369: Instant add columns for innodb
#

create database instant_alter;
use instant_alter;

set timestamp = 1;
set time_zone='+03:00';

eval CREATE TABLE `t1` ( `id` int(11) NOT NULL, `c2` int(11) DEFAULT NULL, PRIMARY KEY (`id`)) ENGINE=InnoDB ROW_FORMAT=$row_format;
 
insert into t1 (id, c2) values(1,1);
select * from t1;

# instant alter
alter table t1 add column (
  `d1` int(11) DEFAULT NULL,
  `d2` int(11) DEFAULT 10,
  `d3` varchar(20) NOT NULL DEFAULT 'abcde',
  `d4` timestamp NOT NULL DEFAULT current_timestamp());
  
select * from t1;
  
insert into t1 (id) values(2),(3),(4),(5),(6);

# add virtural columns
alter table t1 add column `e1` int(11) AS (`id` * 3);
alter table t1 add column `e2` varchar(30) AS (d3);
alter table t1 add column `e3` int(11) AS (`id` * 2);

# instant alter
alter table t1 add column (d5 varchar(20) default 'hijkl', d6 int default 12345, d7 int );

#select * from information_schema.innodb_sys_tables; 
#select * from information_schema.innodb_sys_columns_added; 
#select * from information_schema.innodb_sys_columns_added a, information_schema.innodb_sys_tables b where a.table_id = b.table_id and b.table_name like '%t1%';

# non-inplace update
update t1 set d3 = 'yyyyy' where id = 1;

# inplace update
update t1 set d3 = 'xxxxx' where id = 2;


# transaction rollback
begin;
update t1 set d3 = 'xxxxx' where id = 3;
select * from t1 where id = 3;
rollback;
select * from t1 where id = 3;

# null to null, no change
begin;
update t1 set d7 = null where id = 5; 
rollback;
begin;
update t1 set d7 = null, d6 = 10 where id = 5;
select * from t1 where id = 5;
rollback;
select * from t1 where id = 5;

# add virtual stored columns, copy data
alter table t1 add column (f1 varchar(20) as (concat('x', e2)) stored);

# instant add
alter table t1 add column (d8 varchar(20) default 'omnopq');

select * from t1;



eval create table t2 (id int primary key, c1 varchar(4000), c2 varchar(4000), c3 varchar(1000)) ENGINE=InnoDB ROW_FORMAT=$row_format;

insert into t2 values(1, repeat('a', 4000), repeat('b', 4000), repeat('c', 1));
insert into t2 values(2, repeat('a', 4000), repeat('b', 4000), repeat('c', 1));

alter table t2 add column d1 varchar(2000) default 'fdsafadfdasfafgadsgdasfdassfdasfdassfdasfdassfdassfdasfdasfdassfdasfdassfdasfdasfdassfdsafdsfdasfdasfdasfdassfdasfdassfs';

select * from t2;

# inplace update, rollback
begin;
update t2 set c1 = repeat('c', 4000) where id = 1;
rollback;

# non-inplace update. Create new external columns when rollback.
begin;
update t2 set c1 = repeat('x', 200) where id = 1;
rollback;

select * from t2;


# alter change default values
eval CREATE TABLE t3(a INT PRIMARY KEY, b INT) ENGINE=InnoDB ROW_FORMAT=$row_format;
INSERT INTO t3 SET a=1;
# instantly add columns
ALTER TABLE t3 ADD COLUMN (c INT, d CHAR(10) NULL DEFAULT 'foo', e INT NOT NULL DEFAULT 42);
INSERT INTO t3 SET a=2;
# change some DEFAULT values of old columns, and add one column
ALTER TABLE t3 CHANGE COLUMN b b INT DEFAULT 5,
CHANGE COLUMN c c INT DEFAULT 10,
CHANGE COLUMN d d char(10) DEFAULT NULL;
alter table t3 ADD COLUMN f INT UNSIGNED DEFAULT 0;
INSERT INTO t3 SET a=3;
# change DEFAULTs in .frm file only (nothing inside InnoDB)
ALTER TABLE t3 
CHANGE COLUMN a a INT NOT NULL DEFAULT 101,
CHANGE COLUMN b b INT DEFAULT 102,
CHANGE COLUMN c c INT DEFAULT 103,
CHANGE COLUMN d d char(10) DEFAULT 'eleventy',
CHANGE COLUMN e e INT NOT NULL DEFAULT 105,
CHANGE COLUMN f f INT UNSIGNED DEFAULT 106;
SELECT * FROM t3;

# datetime
create table t4 (id int primary key, c2 int);
insert into t4 values(1,1),(2,2),(3,3);
alter table t4 add column (c3 datetime default current_timestamp(), c4 timestamp not null default current_timestamp());
select * from t4;
alter table t4 add column c5 time not null default current_timestamp();
alter table t4 add column c6 date not null default current_timestamp();
select * from t4;

# add bolb column 
eval create table bug53592(a int) engine=innodb row_format=compact ROW_FORMAT=$row_format;
insert into bug53592 values(1);
alter table bug53592 add column b text charset utf8;
alter table bug53592 add column c blob not null;
select * from bug53592;


# big table
create table big_table (id int primary key, c1 varchar(4000), c2 varchar(4000), c3 varchar(1000));
insert into big_table values(1, repeat('a', 200), repeat('b', 200), repeat('c', 159));
set @i:=1;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table select @i:=@i+1, c1, c2, c3 from big_table;
alter table big_table add column (d1 int default 0, d2 varchar(20) default 'abcde', d3 timestamp not null default current_timestamp on update current_timestamp);
insert into big_table(id, c1, c2, c3) select @i:=@i+1, c1, c2, c3 from big_table;
insert into big_table(id, c1, c2, c3) select @i:=@i+1, c1, c2, c3 from big_table;
checksum table big_table;



drop database instant_alter;
