#
# Helper test file to ensure that an event running on a slave which executes
# for longer than slave_max_statement_time will time out. By default, it will
# use the sleep function to imitate a long running function. This can be
# changed to use locks using the parameter with_lock.
#
# Parameters:
#   with_lock (boolean, in) : Changes the long running command from using SLEEP
#       to using locks. In particular, the MTR test will take the table level
#       write lock on the slave side, while the slave concurrently tries to
#       execute an insert statement.
#

--connection master
CREATE TABLE t1 (a int);
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_slave_max_statement_time=@@GLOBAL.slave_max_statement_time;
SET GLOBAL slave_max_statement_time=0.75;
--let $old_statement_timeouts= query_get_value(SHOW ALL SLAVES STATUS, Slave_statement_timeouts, 1)

if ($with_lock)
{
    LOCK TABLES t1 WRITE;
}

--connection master
if (!$with_lock)
{
    --echo # Mimic a naturally long running command using SLEEP
    INSERT INTO t1 SELECT SLEEP(1);
}
if ($with_lock)
{
    --echo # Long running command due to a lock conflict
    INSERT INTO t1 VALUES (1);
}
--source include/save_master_gtid.inc

--connection slave

--echo # Starting slave to receive event which will take longer to execute
--echo # than slave_max_statement_time
START SLAVE;
--let $slave_sql_errno= 4174
--source include/wait_for_slave_sql_error.inc

--echo # Ensuring event was not processed..
--let $t1_count= `select count(*) from t1`
if ($t1_count != 0)
{
    --die Event should have timed out on the slave and not been executed
}
--echo # ..success

--echo # Ensuring Slave_statement_timeouts variable has been incremented..
--let $new_statement_timeouts= query_get_value(SHOW ALL SLAVES STATUS, Slave_statement_timeouts, 1)
if (`SELECT $new_statement_timeouts != ($old_statement_timeouts+1)`)
{
    --die Timed out statement should have incremented Slave_statement_timeouts
}
--echo # ..success

if ($with_lock)
{
    UNLOCK TABLES;
}

--echo # Remove slave timeout and catch up to master
SET GLOBAL slave_max_statement_time=0;
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
