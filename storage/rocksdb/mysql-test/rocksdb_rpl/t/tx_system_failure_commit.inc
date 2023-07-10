connection master;
create table t1 (id int primary key, value int) engine = rocksdb;

insert into t1 values (1,1),(2,2),(3,3);

# auto commit failure at engine commit, crash and restart (roll forward with binlog)
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
eval set session debug= $myrocks_debug_set_code;
--error 2013
insert into t1 values (4,4);

--enable_reconnect
--source include/wait_until_connected_again.inc

# Primary instance restart needs this, otherwise rpl_end fails.
--let $rpl_server_number = 1
--source include/rpl_reconnect.inc

select * from t1 order by id;
source include/sync_slave_sql_with_master.inc;

connection slave;
select * from t1 order by id;


connection master;
# tx failure at engine commit, crash and restart (roll forward with binlog)
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
eval set session debug= $myrocks_debug_set_code;
begin;
insert into t1 values (5,5);
insert into t1 values (6,6);
--error 2013
commit;

--enable_reconnect
--source include/wait_until_connected_again.inc

# Primary instance restart needs this, otherwise rpl_end fails.
--let $rpl_server_number = 1
--source include/rpl_reconnect.inc

select * from t1 order by id;
source include/sync_slave_sql_with_master.inc;

connection slave;
select * from t1 order by id;


connection master;
drop table t1;
source include/sync_slave_sql_with_master.inc;
