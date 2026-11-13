# Invoked from parallel_backup.test
# Parameters:
# $complete        = COMMIT or ROLLBACK

# This test conducts T1,XAP2,T3,XAC4 parallel execution with retries
# having a BACKUP command interjecting with its BLOCK_COMMIT prior to
# T1 gets out of its retry and T3 has acquire MDL lock.
# We prove the BACKUP command would not prevent 1,2,3 seq-no team group-commit
# as well as itself complete leaving the 4th seq-no transaction waiting.
--let $kind = Normal
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

# the slave group commit leader transaction will be harassed by above into retry
connection master1;
# conflicting value
 --eval INSERT INTO t1 VALUES ($val_1)
--source include/save_master_gtid.inc
--let $save_gtid_1=$master_pos

--connection master
XA START '1';
--eval INSERT INTO  t1 VALUES ($val_0)
XA END '1';
XA PREPARE '1';

--connection master1
--source include/save_master_gtid.inc
--let $save_gtid_2=$master_pos
--eval INSERT INTO t1 VALUES ($val_1 + 1)

--connection master
--eval XA $complete '1'
--source include/save_master_gtid.inc
--let $save_gtid_all=$master_pos

--connection slave
SET @sav_innodb_lock_wait_timeout  = @@global.innodb_lock_wait_timeout;
SET @sav_slave_transaction_retries = @@global.slave_transaction_retries;
SET @@global.innodb_lock_wait_timeout =2;
SET @@global.slave_transaction_retries=100;

--source include/start_slave.inc
# XAP has not acquired BACKUP MDL at waiting
--connection aux_slave
--let $wait_condition= SELECT COUNT(*) = 2 FROM information_schema.processlist WHERE state = "Waiting for prior transaction to commit"
--source include/wait_condition.inc
--echo # Xid '1' must be *not* yet in the output:
XA RECOVER;

--connection backup_slave
  BACKUP STAGE START;
--send BACKUP STAGE BLOCK_COMMIT

--connection aux_slave
  let $status_var= Slave_retried_transactions;
  let $status_var_value= query_get_value(SHOW STATUS LIKE '$status_var', Value, 1);
  # wait for T1's re-try get stuck in repeating wait-for-lock..
  --let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = "Update"
  source include/wait_condition.inc;

  let $status_var_comparsion= >;
  source include/wait_for_status_var.inc;
  # T3 does not let the backup's BLOCK_COMMIT, so the latter is waiting
  --let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = "Waiting for backup lock" and command = "Query"

  # release T1 which is going to
  # - leapfrog the backup thread waiting strong lock at retry
  # - commit itself, notify XAP2 and T3 that both will share MDL and
  #   group-commit.
  ROLLBACK;

  # Backup will finally get the lock; XAC4 is waiting
  --connection aux_slave
  --let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = "Waiting for backup lock" and command = "Slave_worker"
  source include/wait_condition.inc;
  # Unlike --log-bin setup, thanks to T3 timeing to thake the shared MD,
  # XAP2 will also acquire the BACKUP MDL and complete with its gtid_seq_no
  # insertion part.
  --let $master_pos=$save_gtid_2
  --source include/sync_with_master_gtid.inc

  --connection backup_slave
  --reap
  BACKUP STAGE END;

  --connection slave
  --let $master_pos=$save_gtid_all
  --source include/sync_with_master_gtid.inc

--source include/stop_slave.inc

  --connection slave
  SET @@global.innodb_lock_wait_timeout = @sav_innodb_lock_wait_timeout;
  SET @@global.slave_transaction_retries= @sav_slave_transaction_retries;
