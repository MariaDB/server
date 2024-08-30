--source include/have_s3.inc
--source include/have_sequence.inc

#
# Tests for S3 replication
#

sync_slave_with_master;
let $SLAVE_DATADIR= `select @@datadir`;
connection master;

#
# Create unique database for running the tests
#
--source create_database.inc

--echo #
--echo # Test ALTER TABLE ENGINE S3
--echo #

create table t1 (a int, b int) engine=aria;
insert into t1 select seq,seq+10 from seq_1_to_10;
sync_slave_with_master;
connection master;
alter table t1 engine=s3;
show create table t1;

sync_slave_with_master;
--replace_result $database database
--eval use $database
select * from t1 limit 2;
--file_exists $SLAVE_DATADIR/$database/t1.frm

connection master;
alter table t1 add column c int;

sync_slave_with_master;
--error 1
--file_exists $SLAVE_DATADIR/$database/t1.frm
--replace_result $database database
select * from t1,t1 as t1_tmp limit 2;

--echo # Now test when the .frm table is out of date on the slave
stop slave;

connection master;
alter table t1 add column d int, engine=s3;
connection slave;
select * from t1 limit 2;
start slave;
connection master;
sync_slave_with_master;
select * from t1 limit 2;

--echo # Same without tables in the table cache;
stop slave;
flush tables;
connection master;
alter table t1 add column e int, engine=s3;
connection slave;
select * from t1 limit 2;
start slave;
connection master;
sync_slave_with_master;
select * from t1 limit 2;
connection master;

--echo # Convert S3 table to Aria. Rows should be binary logged
alter table t1 engine=aria;
sync_slave_with_master;
select * from t1 limit 2;
show create table t1;

--echo # Convert S3 table to Aria with rename. Rows should be binary logged
connection master;
alter table t1 engine=s3;
alter table t1 rename t2, engine=aria;
sync_slave_with_master;
select * from t2 limit 2;
show create table t2;

connection master;
drop table t2;
sync_slave_with_master;
connection master;

--echo #
--echo # Test RENAME
--echo #

create table t1 (a int, b int) engine=aria;
insert into t1 select seq,seq+10 from seq_1_to_10;
alter table t1 engine=s3;

rename table t1 to t2;
sync_slave_with_master;
--replace_result $database database
--error 1
--file_exists $SLAVE_DATADIR/$database/t2.frm
--error ER_NO_SUCH_TABLE
select * from t1 limit 2;
select * from t2 limit 2;
connection master;

alter table t2 add column f int, rename t1;
select * from t1 limit 2;
sync_slave_with_master;
--error 1
--file_exists $SLAVE_DATADIR/$database/t1.frm
--error 1
--file_exists $SLAVE_DATADIR/$database/t2.frm
select * from t1 limit 2;
--replace_result $database database
--error ER_NO_SUCH_TABLE
select * from t2 limit 2;

# Check rename of table when a new table has replaced the original one

connection slave;
stop slave;
connection master;
rename table t1 to t2;
# Check the different create options with the table
create table t1 (a int) engine=aria;
drop table t1;
create table if not exists t1 (a int, b int) engine=aria;
drop table t1;
create or replace table t1 (a int, b int, c int) engine=aria;
alter table t1 engine=s3;
connection slave;
start slave;
connection master;
sync_slave_with_master;
show create table t1;
select * from t1 limit 2;
select * from t2 limit 2;
connection master;

--echo #
--echo # Test DROP
--echo #
drop table t1,t2;

sync_slave_with_master;
--error 1
--file_exists $SLAVE_DATADIR/$database/t1.frm
--error 1
--file_exists $SLAVE_DATADIR/$database/t2.frm
--replace_result $database database
--error ER_NO_SUCH_TABLE
select * from t1 limit 2;
--replace_result $database database
--error ER_NO_SUCH_TABLE
select * from t2 limit 2;

connection master;

--echo #
--echo # Test LIKE
--echo #

create table t1 (a int,b int);
alter table t1 engine=s3;
--replace_result $database database
--error ER_CANT_CREATE_TABLE
create table t2 like t1;
sync_slave_with_master;
--replace_result $database database
--error ER_NO_SUCH_TABLE
show create table t2;
connection master;
--replace_result $database database
drop table if exists t1,t2;

--echo #
--echo # Check slave binary log
--echo #

sync_slave_with_master;
--let $binlog_database=$database
--source include/show_binlog_events.inc
connection master;

--echo #
--echo # MDEV-24351: S3, same-backend replication: Dropping a table on master
--echo #             causes error on slave
--echo #
show variables like 's3_replicate_alter_as_create_select';

connection slave;
create table t3 (a int, b int) engine=aria;
insert into t3 values (1,1),(2,2),(3,3);
alter table t3 engine=s3;

connection master;
let $binlog_start= query_get_value("SHOW MASTER STATUS", Position, 1);
drop table t3;
--echo # Must show "DROP TABLE IF EXISTS t3", not just "DROP TABLE t3"
--source include/show_binlog_events.inc

sync_slave_with_master;
connection master;

--echo #
--echo # clean up
--echo #
--source drop_database.inc
sync_slave_with_master;
--source include/rpl_end.inc
