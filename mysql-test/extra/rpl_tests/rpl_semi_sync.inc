#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

source include/have_semisync.inc;
source include/not_embedded.inc;
source include/have_innodb.inc;
source include/master-slave.inc;

let $engine_type= InnoDB;

# Suppress warnings that might be generated during the test
connection master;
call mtr.add_suppression("Timeout waiting for reply of binlog");
call mtr.add_suppression("Read semi-sync reply");
call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT.");
connection slave;
call mtr.add_suppression("Master server does not support semi-sync");
call mtr.add_suppression("Semi-sync slave .* reply");
call mtr.add_suppression("Slave SQL.*Request to stop slave SQL Thread received while applying a group that has non-transactional changes; waiting for completion of the group");
connection master;

# wait for dying connections (if any) to disappear
let $wait_condition= select count(*) = 0 from information_schema.processlist where command='killed';
--source include/wait_condition.inc

# After fix of BUG#45848, semi-sync slave should not create any extra
# connections on master, save the count of connections before start
# semi-sync slave for comparison below.
let $_connections_normal_slave= query_get_value(SHOW STATUS LIKE 'Threads_connected', Value, 1);

--echo #
--echo # Uninstall semi-sync plugins on master and slave
--echo #
connection slave;
source include/stop_slave.inc;
reset slave;
set global rpl_semi_sync_master_enabled= 0;
set global rpl_semi_sync_slave_enabled= 0;

connection master;
reset master;
set global rpl_semi_sync_master_enabled= 0;
set global rpl_semi_sync_slave_enabled= 0;

--echo #
--echo # Main test of semi-sync replication start here
--echo #

connection master;

set global rpl_semi_sync_master_timeout= 60000; # 60s

echo [ default state of semi-sync on master should be OFF ];
show variables like 'rpl_semi_sync_master_enabled';

echo [ enable semi-sync on master ];
set global rpl_semi_sync_master_enabled = 1;
show variables like 'rpl_semi_sync_master_enabled';

echo [ status of semi-sync on master should be ON even without any semi-sync slaves ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_yes_tx';

--echo #
--echo # BUG#45672 Semisync repl: ActiveTranx:insert_tranx_node: transaction node allocation failed
--echo # BUG#45673 Semisynch reports correct operation even if no slave is connected
--echo #

# BUG#45672 When semi-sync is enabled on master, it would allocate
# transaction node even without semi-sync slave connected, and would
# finally result in transaction node allocation error.
#
# Semi-sync master will pre-allocate 'max_connections' transaction
# nodes, so here we do more than that much transactions to check if it
# will fail or not.
# select @@global.max_connections + 1;
let $i= `select @@global.max_connections + 1`;
disable_query_log;
eval create table t1 (a int) engine=$engine_type;
while ($i)
{
  eval insert into t1 values ($i);
  dec $i;
}
drop table t1;
enable_query_log;

# BUG#45673
echo [ status of semi-sync on master should be OFF ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_yes_tx';

# reset master to make sure the following test will start with a clean environment
reset master;

connection slave;

echo [ default state of semi-sync on slave should be OFF ];
show variables like 'rpl_semi_sync_slave_enabled';

echo [ enable semi-sync on slave ];
set global rpl_semi_sync_slave_enabled = 1;
show variables like 'rpl_semi_sync_slave_enabled';
source include/start_slave.inc;

connection master;

# NOTE: Rpl_semi_sync_master_client will only be updated when
# semi-sync slave has started binlog dump request
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 1;
source include/wait_for_status_var.inc;

echo [ initial master state after the semi-sync slave connected ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

replace_result $engine_type ENGINE_TYPE;
eval create table t1(a int) engine = $engine_type;

echo [ master state after CREATE TABLE statement ];
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

# After fix of BUG#45848, semi-sync slave should not create any extra
# connections on master.
let $_connections_semisync_slave= query_get_value(SHOW STATUS LIKE 'Threads_connected', Value, 1);
replace_result $_connections_normal_slave CONNECTIONS_NORMAL_SLAVE $_connections_semisync_slave CONNECTIONS_SEMISYNC_SLAVE;
eval select $_connections_semisync_slave - $_connections_normal_slave as 'Should be 0';

echo [ insert records to table ];
insert t1 values (10);
insert t1 values (9);
insert t1 values (8);
insert t1 values (7);
insert t1 values (6);
insert t1 values (5);
insert t1 values (4);
insert t1 values (3);
insert t1 values (2);
insert t1 values (1);

echo [ master status after inserts ];
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

sync_slave_with_master;

echo [ slave status after replicated inserts ];
show status like 'Rpl_semi_sync_slave_status';

select count(distinct a) from t1;
select min(a) from t1;
select max(a) from t1;

--echo
--echo # BUG#50157
--echo # semi-sync replication crashes when replicating a transaction which
--echo # include 'CREATE TEMPORARY TABLE `MyISAM_t` SELECT * FROM `Innodb_t` ;

connection master;
SET SESSION AUTOCOMMIT= 0;
CREATE TABLE t2(c1 INT) ENGINE=innodb;
sync_slave_with_master;

connection master;
BEGIN;
--echo
--echo # Even though it is in a transaction, this statement is binlogged into binlog
--echo # file immediately.
--disable_warnings
CREATE TEMPORARY TABLE t3 SELECT c1 FROM t2 where 1=1;
--enable_warnings
--echo
--echo # These statements will not be binlogged until the transaction is committed
INSERT INTO t2 VALUES(11);
INSERT INTO t2 VALUES(22);
COMMIT;

DROP TABLE t2, t3;
SET SESSION AUTOCOMMIT= 1;
sync_slave_with_master;


--echo #
--echo # Test semi-sync master will switch OFF after one transaction
--echo # timeout waiting for slave reply.
--echo #
connection slave;
source include/stop_slave.inc;

connection master;
--source include/kill_binlog_dump_threads.inc
set global rpl_semi_sync_master_timeout= 5000;

# The first semi-sync check should be on because after slave stop,
# there are no transactions on the master.
echo [ master status should be ON ];

let $status_var= Rpl_semi_sync_master_status;
let $status_var_value= ON;
source include/wait_for_status_var.inc;

let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 0;
source include/wait_for_status_var.inc;

show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

echo [ semi-sync replication of these transactions will fail ];
insert into t1 values (500);

# Wait for the semi-sync replication of this transaction to timeout
let $status_var= Rpl_semi_sync_master_status;
let $status_var_value= OFF;
source include/wait_for_status_var.inc;

# The second semi-sync check should be off because one transaction
# times out during waiting.
echo [ master status should be OFF ];
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

# Semi-sync status on master is now OFF, so all these transactions
# will be replicated asynchronously.
delete from t1 where a=10;
delete from t1 where a=9;
delete from t1 where a=8;
delete from t1 where a=7;
delete from t1 where a=6;
delete from t1 where a=5;
delete from t1 where a=4;
delete from t1 where a=3;
delete from t1 where a=2;
delete from t1 where a=1;

insert into t1 values (100);

echo [ master status should be OFF ];
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

--echo #
--echo # Test semi-sync status on master will be ON again when slave catches up
--echo #

# Save the master position for later use.
save_master_pos;

connection slave;

echo [ slave status should be OFF ];
show status like 'Rpl_semi_sync_slave_status';
source include/start_slave.inc;
sync_with_master;

echo [ slave status should be ON ];
show status like 'Rpl_semi_sync_slave_status';

select count(distinct a) from t1;
select min(a) from t1;
select max(a) from t1;

connection master;

# The master semi-sync status should be on again after slave catches up.
echo [ master status should be ON again after slave catches up ];

let $status_var= Rpl_semi_sync_master_status;
let $status_var_value= ON;
source include/wait_for_status_var.inc;
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';
show status like 'Rpl_semi_sync_master_clients';

--echo #
--echo # Test disable/enable master semi-sync on the fly.
--echo #

drop table t1;
sync_slave_with_master;

source include/stop_slave.inc;

--echo #
--echo # Flush status
--echo #
connection master;
echo [ Semi-sync master status variables before FLUSH STATUS ];
SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx';
SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx';
# Do not write the FLUSH STATUS to binlog, to make sure we'll get a
# clean status after this.
FLUSH NO_WRITE_TO_BINLOG STATUS;
echo [ Semi-sync master status variables after FLUSH STATUS ];
SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx';
SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx';

connection master;

source include/show_master_logs.inc;
show variables like 'rpl_semi_sync_master_enabled';

echo [ disable semi-sync on the fly ];
set global rpl_semi_sync_master_enabled=0;
show variables like 'rpl_semi_sync_master_enabled';
show status like 'Rpl_semi_sync_master_status';

echo [ enable semi-sync on the fly ];
set global rpl_semi_sync_master_enabled=1;
show variables like 'rpl_semi_sync_master_enabled';
show status like 'Rpl_semi_sync_master_status';

--echo #
--echo # Test RESET MASTER/SLAVE
--echo #

connection slave;

source include/start_slave.inc;

connection master;

replace_result $engine_type ENGINE_TYPE;
eval create table t1 (a int) engine = $engine_type;
drop table t1;

sync_slave_with_master;

echo [ test reset master ];
connection master;

reset master;

show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

connection slave;

source include/stop_slave.inc;
reset slave;

# Kill the dump thread on master for previous slave connection and
--source include/kill_binlog_dump_threads.inc

connection slave;
source include/start_slave.inc;

connection master;

# Wait for dump thread to start, Rpl_semi_sync_master_clients will be
# 1 after dump thread started.
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 1;
source include/wait_for_status_var.inc;

replace_result $engine_type ENGINE_TYPE;
eval create table t1 (a int) engine = $engine_type;
insert into t1 values (1);
insert into t1 values (2), (3);

sync_slave_with_master;

select * from t1;

connection master;

echo [ master semi-sync status should be ON ];
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

--echo #
--echo # Start semi-sync replication without SUPER privilege
--echo #
connection slave;
source include/stop_slave.inc;
reset slave;
connection master;
reset master;

# Kill the dump thread on master for previous slave connection and wait for it to exit
--source include/kill_binlog_dump_threads.inc

# Do not binlog the following statement because it will generate
# different events for ROW and STATEMENT format
set sql_log_bin=0;
grant replication slave on *.* to rpl@127.0.0.1 identified by 'rpl_password';
flush privileges;
set sql_log_bin=1;
connection slave;
grant replication slave on *.* to rpl@127.0.0.1 identified by 'rpl_password';
flush privileges;
change master to master_user='rpl',master_password='rpl_password';
source include/start_slave.inc;
show status like 'Rpl_semi_sync_slave_status';
connection master;

# Wait for the semi-sync binlog dump thread to start
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 1;
source include/wait_for_status_var.inc;
echo [ master semi-sync should be ON ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';
insert into t1 values (4);
insert into t1 values (5);
echo [ master semi-sync should be ON ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_no_tx';
show status like 'Rpl_semi_sync_master_yes_tx';

--echo #
--echo # Test semi-sync slave connect to non-semi-sync master
--echo #

# Disable semi-sync on master
connection slave;
source include/stop_slave.inc;
SHOW STATUS LIKE 'Rpl_semi_sync_slave_status';

connection master;

# Kill the dump thread on master for previous slave connection and wait for it to exit
--source include/kill_binlog_dump_threads.inc

echo [ Semi-sync status on master should be ON ];
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 0;
source include/wait_for_status_var.inc;
show status like 'Rpl_semi_sync_master_status';
let $status_var= Rpl_semi_sync_master_status;
let $status_var_value= ON;
source include/wait_for_status_var.inc;
set global rpl_semi_sync_master_enabled= 0;

connection slave;
SHOW VARIABLES LIKE 'rpl_semi_sync_slave_enabled';
source include/start_slave.inc;
connection master;
insert into t1 values (8);
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 1;
source include/wait_for_status_var.inc;
echo [ master semi-sync clients should be 1, status should be OFF ];
show status like 'Rpl_semi_sync_master_clients';
show status like 'Rpl_semi_sync_master_status';
sync_slave_with_master;
show status like 'Rpl_semi_sync_slave_status';

# Uninstall semi-sync plugin on master
connection slave;
source include/stop_slave.inc;
connection master;
set global rpl_semi_sync_master_enabled= 0;

connection slave;
SHOW VARIABLES LIKE 'rpl_semi_sync_slave_enabled';
source include/start_slave.inc;

connection master;
insert into t1 values (10);
sync_slave_with_master;

--echo #
--echo # Test non-semi-sync slave connect to semi-sync master
--echo #

connection master;
set global rpl_semi_sync_master_timeout= 5000; # 5s
set global rpl_semi_sync_master_enabled= 1;

connection slave;
source include/stop_slave.inc;
SHOW STATUS LIKE 'Rpl_semi_sync_slave_status';

echo [ uninstall semi-sync slave plugin ];
set global rpl_semi_sync_slave_enabled= 0;

echo [ reinstall semi-sync slave plugin and disable semi-sync ];
SHOW VARIABLES LIKE 'rpl_semi_sync_slave_enabled';
SHOW STATUS LIKE 'Rpl_semi_sync_slave_status';
source include/start_slave.inc;
SHOW STATUS LIKE 'Rpl_semi_sync_slave_status';

--echo #
--echo # Clean up
--echo #

connection slave;
source include/stop_slave.inc;
set global rpl_semi_sync_slave_enabled= 0;

connection master;
set global rpl_semi_sync_master_enabled= 0;

connection slave;
change master to master_user='root',master_password='';
source include/start_slave.inc;

connection master;
drop table t1;
sync_slave_with_master;

connection master;
drop user rpl@127.0.0.1;
flush privileges;
set global rpl_semi_sync_master_timeout= default;
--source include/rpl_end.inc
