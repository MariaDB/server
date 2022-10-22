#
# Helper test file to ensure that an event running on a slave which executes
# for longer than @@slave_max_statement_time will time out. By default, it will
# use the sleep function to imitate a long running function. This can be
# changed to use locks using the parameter with_lock.
#
# Parameters:
#   with_lock (boolean, in) : Changes the long running command from using SLEEP
#       to using locks. In particular, the MTR test will take the table level
#       write lock on the slave side, while the slave concurrently tries to
#       execute an insert statement.
#
#   use_load_data (boolean, in) : If in row logging format, uses LOAD DATA
#       INFILLE command to create Load_log_events to create the events which
#       should time out
#

--connection master
create table t1(a int not null auto_increment, b int, primary key(a)) engine=InnoDB;
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_slave_max_statement_time=@@GLOBAL.slave_max_statement_time;
SET GLOBAL slave_max_statement_time=0.75;

--connection master
--echo # Long running command due to a lock conflict
if (!$use_load_data)
{
   INSERT INTO t1(b) VALUES (1);
}
if ($use_load_data)
{
   load data infile '../../std_data/rpl_loaddata.dat' into table t1;
}
--source include/save_master_gtid.inc

--connection slave1
  BEGIN; INSERT INTO t1(b) VALUES (1);

--connection slave

--echo # Starting slave to receive event which will take longer to execute
--echo # than slave_max_statement_time
START SLAVE;

# ER_SLAVE_STATEMENT_TIMEOUT
--let $slave_sql_errno= 4192
--source include/wait_for_slave_sql_error.inc

--echo # Ensuring event was not processed..
--let $t1_count= `select count(*) from t1`
if ($t1_count != 0)
{
    --die Event should have timed out on the slave and not been executed
}
--echo # ..success

--echo # Remove slave timeout and catch up to master
SET GLOBAL slave_max_statement_time=0;

--connection slave1
  ROLLBACK;

--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

--echo # Test case cleanup
--connection master
DROP TABLE t1;
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET GLOBAL slave_max_statement_time=@old_slave_max_statement_time;
--source include/start_slave.inc
