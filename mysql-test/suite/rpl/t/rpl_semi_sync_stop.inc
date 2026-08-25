#
#   This test sets up a semi-sync master-slave topology, and stops the
# slave IO thread. It then validates that the connection was killed by using
# the wait_condition.inc utility to wait for the binlog dump thread to die,
# and also validates that the status variable Rpl_semi_sync_master_clients
# reports as 0.
#
# Prerequisites:
#   master-slave.inc
#   CREATE TABLE t1 (a INT)
#   connection slave
#   slave stopped
# Post-conditions: slave stopped, connection master
#

--source include/start_slave.inc

--connection master
--echo # Verify Semi-Sync is active
--let $status_var= Rpl_semi_sync_master_clients
--let $status_var_value= 1
--source include/wait_for_status_var.inc
SHOW STATUS LIKE 'Rpl_semi_sync_master_clients';

--sync_slave_with_master

--echo # Disconnect the slave and wait until the master's dump thread is gone
STOP SLAVE;
--connection master

--echo # Write something to wake the dump thread from sleeping for binlog events
INSERT INTO t1 VALUES (0);

--echo # Verifying dump thread connection is killed..
# Prior to fixes, this would time out and
# Rpl_semi_sync_master_clients would remain 1.
--let $wait_condition= SELECT COUNT(*)=0 FROM information_schema.PROCESSLIST WHERE USER = 'replssl'
--source include/wait_condition.inc

--let $n_master_clients= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_clients', Value, 1)
if ($n_master_clients)
{
  --echo # Rpl_semi_sync_master_clients: $n_master_clients
  --die Semi-sync dump thread connection not killed
}
--echo # ..done
