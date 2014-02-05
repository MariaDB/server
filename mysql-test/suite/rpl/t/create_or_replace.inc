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
# This should be logged as we will delete t1
--error ER_DUP_ENTRY
create or replace table t1 (a int primary key) select a from t2;

--echo binlog from server 1
--source include/show_binlog_events.inc
save_master_pos;
connection server_2;
sync_with_master;
show tables;
connection server_1;

drop table if exists t1,t2;

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

# Clean up
drop table t2;

--source include/rpl_end.inc
