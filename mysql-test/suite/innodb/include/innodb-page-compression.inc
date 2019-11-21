# This test is slow on buildbot.
--source include/big_test.inc
--disable_warnings
set global innodb_file_format = `Barracuda`;
set global innodb_file_per_table = on;
--enable_warnings

create table innodb_normal (c1 int not null auto_increment primary key, b char(200)) engine=innodb;
create table innodb_page_compressed1 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=1;
create table innodb_page_compressed2 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=2;
create table innodb_page_compressed3 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=3;
create table innodb_page_compressed4 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=4;
create table innodb_page_compressed5 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=5;
create table innodb_page_compressed6 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=6;
create table innodb_page_compressed7 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=7;
create table innodb_page_compressed8 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=8;
create table innodb_page_compressed9 (c1 int not null auto_increment primary key, b char(200)) engine=innodb page_compressed=1 page_compression_level=9;

--disable_query_log
begin;
let $i = 2000;
while ($i)
{
    insert into innodb_normal(b) values(REPEAT('Aa',50));
    insert into innodb_normal(b) values(REPEAT('a',100));
    insert into innodb_normal(b) values(REPEAT('b',100));
    insert into innodb_normal(b) values(REPEAT('0',100));
    insert into innodb_normal(b) values(REPEAT('1',100));
    dec $i;
}

insert into innodb_page_compressed1 select * from innodb_normal;
insert into innodb_page_compressed2 select * from innodb_normal;
insert into innodb_page_compressed3 select * from innodb_normal;
insert into innodb_page_compressed4 select * from innodb_normal;
insert into innodb_page_compressed5 select * from innodb_normal;
insert into innodb_page_compressed6 select * from innodb_normal;
insert into innodb_page_compressed7 select * from innodb_normal;
insert into innodb_page_compressed8 select * from innodb_normal;
insert into innodb_page_compressed9 select * from innodb_normal;
commit;
--enable_query_log

select count(*) from innodb_page_compressed1;
select count(*) from innodb_page_compressed3;
select count(*) from innodb_page_compressed4;
select count(*) from innodb_page_compressed5;
select count(*) from innodb_page_compressed6;
select count(*) from innodb_page_compressed6;
select count(*) from innodb_page_compressed7;
select count(*) from innodb_page_compressed8;
select count(*) from innodb_page_compressed9;

#
# Wait until pages are really compressed
#
let $wait_condition= select variable_value > 0 from information_schema.global_status where variable_name = 'INNODB_NUM_PAGES_PAGE_COMPRESSED';
--source include/wait_condition.inc

--let $MYSQLD_DATADIR=`select @@datadir`

# shutdown before grep

--source include/shutdown_mysqld.inc

--let t1_IBD = $MYSQLD_DATADIR/test/innodb_normal.ibd
--let SEARCH_RANGE = 10000000
--let SEARCH_PATTERN=AaAaAaAa
--echo # innodb_normal expected FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed1.ibd
--echo # innodb_page_compressed1 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed2.ibd
--echo # innodb_page_compressed2 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed3.ibd
--echo # innodb_page_compressed3 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed4.ibd
--echo # innodb_page_compressed4 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed5.ibd
--echo # innodb_page_compressed5 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed6.ibd
--echo # innodb_page_compressed6 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed7.ibd
--echo # innodb_page_compressed7 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed8.ibd
--echo # innodb_page_compressed8 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc
--let t1_IBD = $MYSQLD_DATADIR/test/innodb_page_compressed9.ibd
--echo # innodb_page_compressed9 page compressed expected NOT FOUND
-- let SEARCH_FILE=$t1_IBD
-- source include/search_pattern_in_file.inc

-- source include/start_mysqld.inc

select count(*) from innodb_page_compressed1;
select count(*) from innodb_page_compressed3;
select count(*) from innodb_page_compressed4;
select count(*) from innodb_page_compressed5;
select count(*) from innodb_page_compressed6;
select count(*) from innodb_page_compressed6;
select count(*) from innodb_page_compressed7;
select count(*) from innodb_page_compressed8;
select count(*) from innodb_page_compressed9;

let $wait_condition= select variable_value > 0 from information_schema.global_status where variable_name = 'INNODB_NUM_PAGES_PAGE_DECOMPRESSED';
--source include/wait_condition.inc

drop table innodb_normal;
drop table innodb_page_compressed1;
drop table innodb_page_compressed2;
drop table innodb_page_compressed3;
drop table innodb_page_compressed4;
drop table innodb_page_compressed5;
drop table innodb_page_compressed6;
drop table innodb_page_compressed7;
drop table innodb_page_compressed8;
drop table innodb_page_compressed9;
