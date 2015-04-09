-- source include/have_xtradb.inc
-- source filekeys_plugin.inc

create table t1(c1 bigint not null, b char(200))  engine=innodb encrypted=yes encryption_key_id=1;
show create table t1;
insert t1 values (12345, repeat('1234567890', 20));

alter table t1 encryption_key_id=2;
show create table t1;
--replace_regex /#sql-[0-9a-f_]*/#sql-temporary/
--error ER_CANT_CREATE_TABLE
alter table t1 encryption_key_id=3;
show create table t1;
alter table t1 encryption_key_id=33;
show create table t1;
alter table t1 encryption_key_id=4;
show create table t1;

drop table t1;

