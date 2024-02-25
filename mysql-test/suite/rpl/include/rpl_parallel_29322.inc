# A "body" for rpl.rpl_parallel_29322 that runs having as input
# the master and slave @@global.explicit_defaults_for_timestamp.
#
# The Key notes
# -------------
# On master: a sequence of binlog files is composed with
#            different server versions.
# On slave:  to first try enqueuing all master events.
#            Due to binlog files are from different version
#            servers when the new (of binlog 2) FD shows up the enqueuing
#            will hiccup as workers have to
#            process all events already scheduled (see wait_condition below).
#            This part proves of the different server version the binlogs
#            indeed are.
#            After the worker is unblocked the exection proceeds.
#
#            In the end compare table definitions and print table data
#            to prove correctness.

--connection slave
--source include/stop_slave.inc
--let $master_use_gtid_option=No
--source include/reset_slave.inc

--connection master
--let $datadir= `SELECT @@datadir`

--source include/kill_binlog_dump_threads.inc
reset master; # Master starts a new serious of binlog files

create table t2 (a timestamp, b int);
show create table t2;
--save_master_pos

--connection slave
--source include/start_slave.inc
--sync_with_master
--source include/stop_slave.inc

# load for two $same_version_binlogs branches
--connection master
if (!$same_version_binlogs)
{
  insert into t2 values (null, 1);
  flush binary logs;
  flush binary logs;
  insert into t2 values (null, 2);
}
if ($same_version_binlogs)
{
--disable_query_log
  --let $count=8
  while ($count)
  {
    --let $queries=4
    while ($queries)
    {
     insert into t2 values (null, 1);
     --dec $queries
    }
    flush binary logs;
    flush binary logs;
    --dec $count
  }
--enable_query_log
}
--save_master_pos

if (!$same_version_binlogs)
{
  # Make sure the dump thread is gone before moving around binlog files. Else
  # it might see an empty file and give error (MDEV-29816).
  --let $dump_thrid= `Select id FROM information_schema.processlist WHERE Command='Binlog Dump'`
  if ($dump_thrid) {
    --disable_query_log
    --error 0,ER_NO_SUCH_THREAD
    eval KILL CONNECTION $dump_thrid;
    --enable_query_log
    --let $wait_condition= SELECT COUNT(*)=0 FROM information_schema.processlist WHERE Command='Binlog Dump'
    --source include/wait_condition.inc
  }
  --move_file $datadir/master-bin.000002 $datadir/master-bin.000002.sav
  --copy_file $MYSQL_TEST_DIR/std_data/mdev29078-mysql-bin.000001 $datadir/master-bin.000002
  --exec $MYSQL_BINLOG --short-form $datadir/master-bin.000002

  # To demonstrate different version binlogs, hold back worker
  # until the SQL thread state shows waiting.
  --connection slave1
    lock table t2 write;
}

--connection slave
--source include/start_slave.inc

if (!$same_version_binlogs)
{
  --let $slave_waits_for=Waiting for worker threads to be idle
  --let $wait_condition= SELECT COUNT(*) > 0 FROM information_schema.processlist WHERE state = "$slave_waits_for"
  --source include/wait_condition.inc

  --connection slave1
    unlock tables;

  --connection slave
  --sync_with_master
  show create table t1;
  set statement time_zone='+2:00' for select * from t1;
  drop table t1;

  --connection master
  --remove_file $datadir/master-bin.000002
  --move_file  $datadir/master-bin.000002.sav $datadir/master-bin.000002
}

--connection slave
--sync_with_master
show create table t2;
--let $diff_tables=master:t2,slave:t2
--source include/diff_tables.inc

--connection master
drop table t2;

--sync_slave_with_master
