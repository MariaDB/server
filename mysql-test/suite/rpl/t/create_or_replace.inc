# Test CREATE OR REPLACE TABLE in replication
--source include/have_innodb.inc

--let $rpl_topology=1->2
--source include/rpl_init.inc

# Create help tables
create table t2 (a int) engine=myisam;
insert into t2 values (0),(1),(2),(2);
create temporary table t3 (a_in_temporary int) engine=myisam;

--echo #
--echo # Check how create table and create or replace table are logged
--echo #

save_master_pos;
connection server_2;
sync_with_master;
create table t1 (to_be_deleted int);

connection server_1;
CREATE TABLE t1 AS SELECT 1 AS f1;
CREATE OR REPLACE TABLE t1 AS SELECT 2 AS f1;
CREATE OR REPLACE table t1 like t2;
CREATE OR REPLACE table t1 like t3;
drop table t1;

--echo binlog from server 1
--source include/show_binlog_events.inc
save_master_pos;
connection server_2;
sync_with_master;
--echo binlog from server 2
--source include/show_binlog_events.inc

connection server_1;

--echo #
--echo # Ensure that also failed create_or_replace are logged
--echo #

--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)

create table t1 (a int);
--error ER_TABLE_MUST_HAVE_COLUMNS
create or replace table t1;
drop table if exists t1;
# The following is not logged as t1 does not exists;
--error ER_DUP_ENTRY
create or replace table t1 (a int primary key) select a from t2;

create table t1 (a int);
# This should as a delete as we will delete t1
--error ER_DUP_ENTRY
create or replace table t1 (a int primary key) select a from t2;

# Same with temporary table
create temporary table t9 (a int);
--error ER_DUP_ENTRY
create or replace temporary table t9 (a int primary key) select a from t2;

--echo binlog from server 1
--source include/show_binlog_events.inc
save_master_pos;
connection server_2;
sync_with_master;
show tables;
connection server_1;

--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)
create table t1 (a int);
--error ER_DUP_FIELDNAME
create or replace table t1 (a int, a int) select * from t2;
--source include/show_binlog_events.inc

drop table if exists t1,t2;
drop temporary table if exists t9;

--echo #
--echo # Ensure that CREATE are run as CREATE OR REPLACE on slave
--echo #

save_master_pos;
connection server_2;
sync_with_master;
create table t1 (server_2_to_be_delete int);
connection server_1;
create table t1 (new_table int);

save_master_pos;
connection server_2;
sync_with_master;

show create table t1;
connection server_1;
drop table t1;

--echo #
--echo # Check how CREATE is logged on slave in case of conflicts
--echo #

save_master_pos;
connection server_2;
sync_with_master;
--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)
create table t1 (server_2_to_be_delete int);
create table t2 (server_2_to_be_delete int);
create table t4 (server_2_to_be_delete int);
set @org_binlog_format=@@binlog_format;
set @@global.binlog_format="ROW";
stop slave;
--source include/wait_for_slave_to_stop.inc
start slave;
--source include/wait_for_slave_to_start.inc
connection server_1;
create temporary table t9 (a int);
insert into t9 values(1);
create table t1 (new_table int);
create table t2 select * from t9;
create table t4 like t9;
create table t5 select * from t9;
save_master_pos;
connection server_2;
sync_with_master;
--echo binlog from server 2
--source include/show_binlog_events.inc
set @@global.binlog_format=@org_binlog_format;
stop slave;
--source include/wait_for_slave_to_stop.inc
start slave;
--source include/wait_for_slave_to_start.inc
connection server_1;
drop table t1,t2,t4,t5,t9;

--echo #
--echo # Ensure that DROP TABLE is run as DROP IF NOT EXISTS
--echo #

create table t1 (server_1_ver_1 int);
create table t4 (server_1_ver_2 int);

save_master_pos;
connection server_2;
sync_with_master;
--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)

# Drop the table on the slave
drop table t1;
connection server_1;
drop table t1,t4;
create table t1 (server_2_ver_2 int);
save_master_pos;
connection server_2;
sync_with_master;
show create table t1;
--echo binlog from server 2
--source include/show_binlog_events.inc
connection server_1;
drop table t1;

--echo #
--echo # Ensure that CREATE ... SELECT is recorded as one GTID on the slave
--echo #

save_master_pos;
connection server_2;
sync_with_master;
--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)
connection server_1;

create table t1 (a int);
insert into t1 values (0),(1),(2);
create table t2 engine=myisam select * from t1;
create or replace table t2 engine=innodb select * from t1;
save_master_pos;
connection server_2;
sync_with_master;
--echo binlog from server 2
--source include/show_binlog_events.inc
connection server_1;
drop table t1;

--echo #
--echo # Check logging of drop temporary table
--echo #

drop temporary table t3;

--let $binlog_start=query_get_value(SHOW MASTER STATUS, Position, 1)

set @org_binlog_format=@@binlog_format;
set binlog_format="STATEMENT";
create temporary table t5 (a int);
drop temporary table t5;
set binlog_format="ROW";
create temporary table t6 (a int);
drop temporary table t6;
set binlog_format="STATEMENT";
create temporary table t7 (a int);
set binlog_format="ROW";
drop temporary table t7;
create temporary table t8 (a int);
--error ER_TEMP_TABLE_PREVENTS_SWITCH_OUT_OF_RBR
set binlog_format="STATEMENT";
drop temporary table t8;
set @@binlog_format=@org_binlog_format;

# MDEV-20091:
# 1. No DROP should be logged for non-existing tmp table, nor
# 2. at the connection close when its creation has not been logged.
set @@session.binlog_format=default;
drop temporary table if exists t9;

--connect(con1,localhost,root,,)
set session binlog_format=default;
create temporary table t9 (i int);
--echo *** Must be no DROP logged for t9 when there was no CREATE, at disconnect too ***
--disconnect con1

--connection server_1
--source include/show_binlog_events.inc

# Clean up
drop table t2;

--source include/rpl_end.inc
