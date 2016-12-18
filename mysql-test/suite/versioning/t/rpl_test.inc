#BUG#12662190 - COM_COMMIT IS NOT INCREMENTED FROM THE BINARY LOGS ON SLAVE, COM_BEGIN IS
#Testing command counters -BEFORE.
#Storing the before counts of Slave
connection slave;
let $slave_com_commit_before= query_get_value(SHOW GLOBAL STATUS LIKE 'com_commit', Value, 1);
let $slave_com_insert_before= query_get_value(SHOW GLOBAL STATUS LIKE 'com_insert', Value, 1);
let $slave_com_delete_before= query_get_value(SHOW GLOBAL STATUS LIKE 'com_delete', Value, 1);
let $slave_com_update_before= query_get_value(SHOW GLOBAL STATUS LIKE 'com_update', Value, 1);

connection master;
CREATE TABLE t1 (x int) with system versioning ENGINE = innodb;
insert into t1 values (1);
SELECT * FROM t1;
delete from t1;
select * from t1;
select * from t1 for system_time all;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

connection master;
insert into t1 values (2);
sync_slave_with_master;
select * from t1;

connection master;
update t1 set x = 3;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

# check unversioned -> versioned replication
connection master;
create or replace table t1 (x int primary key) engine = innodb;
sync_slave_with_master;
alter table t1 with system versioning;

connection master;
insert into t1 values (1);
update t1 set x= 2 where x = 1;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

connection master;
delete from t1;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

# same thing (UPDATE, DELETE), but without PK
connection master;
create or replace table t1 (x int) engine = innodb;
sync_slave_with_master;
alter table t1 with system versioning;

connection master;
insert into t1 values (1);
update t1 set x= 2 where x = 1;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

connection master;
delete from t1;
sync_slave_with_master;
select * from t1;
select * from t1 for system_time all;

# same thing, but reverse: versioned -> unversioned
connection master;
create or replace table t1 (x int primary key) with system versioning engine = innodb;
sync_slave_with_master;
alter table t1 without system versioning;

connection master;
insert into t1 values (1);
update t1 set x= 2 where x = 1;
select * from t1 for system_time all;
sync_slave_with_master;
select * from t1;

connection master;
delete from t1;
select * from t1 for system_time all;
sync_slave_with_master;
select * from t1;

connection master;
drop table t1;
