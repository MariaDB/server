# Run the load generator to populate the table and generate concurrent
# updates.  After the load generator is complete, verify the tables on the
# master and the slave are consistent

--sync_slave_with_master

--connection master
--let $master_host = 127.0.0.1
let $MYSQL_BASEDIR = `SELECT @@BASEDIR`;

let $exec =
  python
  $MYSQL_BASEDIR/storage/rocksdb/mysql-test/rocksdb_stress/t/load_generator.py \
  -L $MYSQL_TMP_DIR/load_generator.log -H $master_host -t $table \
  -P $MASTER_MYPORT -n $num_records -m $max_records \
  -l $num_loaders -c $num_checkers -r $num_requests \
  -E $MYSQLTEST_VARDIR/tmp/mysqld.1.expect \
  -D $reap_delay;

exec $exec;

enable_reconnect;
source include/wait_until_connected_again.inc;

--let $master_checksum = query_get_value(CHECKSUM TABLE $table, Checksum, 1)

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/wait_for_slave_to_sync_with_master.inc

--connection slave
--let $slave_checksum = query_get_value(CHECKSUM TABLE $table, Checksum, 1)

let $not_same = `SELECT $master_checksum-$slave_checksum`;
if ($not_same)
{
  --die "The checksums of table $table for the master and slave do not match!"
}

# Cleanup
--connection master
--let $cleanup = DROP TABLE $table
eval $cleanup;

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/wait_for_slave_to_sync_with_master.inc

--connection slave
--source include/stop_slave.inc
# For stress tests sometimes the replication thread can not connect to master
# temporarily. This is either because the master crashed and it is recovering
# or the master is too busy and could not service the slave's requests.
# mtr's internal check requires that there be no errors in slave status.
# restarting replication clears the errors.
--source include/start_slave.inc
--source include/stop_slave.inc
