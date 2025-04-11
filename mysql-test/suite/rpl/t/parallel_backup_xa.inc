# Invoked from parallel_backup.test
# Parameters:
# $complete        = COMMIT or ROLLBACK
# $slave_ooo_error = 1 means slave group commit did not succeed
#
--let $kind = Normal
if ($slave_ooo_error)
{
  --let $kind = Errored out
}
--echo #
--echo # $kind XA $complete

--connection slave
--source include/stop_slave.inc

--connection master
# val_0 is the first value to insert on master in prepared xa
# val_1 is the next one to insert which is the value to block on slave
--let $val_0 = `SELECT max(a)+1 FROM t1`
--let $val_1 = $val_0
--inc $val_1

--connection aux_slave
BEGIN;
--eval INSERT INTO t1 VALUES ($val_1)

--connection master
XA START '1';
--eval INSERT INTO  t1 VALUES ($val_0)
XA END '1';
XA PREPARE '1';
--connection master1
--eval INSERT INTO t1 VALUES ($val_1)
--connection master
--eval XA $complete '1'
--source include/save_master_gtid.inc

--connection backup_slave
BACKUP STAGE START;

--connection slave
if ($slave_ooo_error)
{
  SET @sav_innodb_lock_wait_timeout  = @@global.innodb_lock_wait_timeout;
  SET @sav_slave_transaction_retries = @@global.slave_transaction_retries;
  SET @@global.innodb_lock_wait_timeout =5;
  SET @@global.slave_transaction_retries=0;
}
--source include/start_slave.inc
--connection aux_slave
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = "Waiting for prior transaction to commit"
--source include/wait_condition.inc
--echo # Xid '1' must be in the output:
XA RECOVER;
--connection backup_slave
--send BACKUP STAGE BLOCK_COMMIT
--connection aux_slave
  --sleep 1
  if ($slave_ooo_error)
  {
    --let $wait_condition= SELECT COUNT(*) = 0 FROM information_schema.processlist WHERE state = "Waiting for prior transaction to commit"
    --source include/wait_condition.inc
    --let $rpl_allow_error=1
  }
  ROLLBACK;
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = "Waiting for backup lock"
--source include/wait_condition.inc
--connection backup_slave
  --reap
  BACKUP STAGE END;
--connection slave
if (!$slave_ooo_error)
{
  --source include/sync_with_master_gtid.inc
}  
--let $rpl_only_running_threads= 1
--source include/stop_slave.inc
if ($slave_ooo_error)
{
  SET @@global.innodb_lock_wait_timeout = @sav_innodb_lock_wait_timeout;
  SET @@global.slave_transaction_retries= @sav_slave_transaction_retries;
}
