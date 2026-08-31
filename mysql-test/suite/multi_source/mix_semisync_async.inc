# Test replicating from an additional connection of semi-sync mode `@mode2`
# does not interfere with the active connection of mode `@mode1`.
#
# Pre-Conditions:
# * On connection `slave`.
# * CHANGE MASTER `master1` and `master2` set up to
#   replicate from the connection with the same name.
# * None of the replication threads are running.
# * User variables `@mode1` and `@mode2` contain
#   corresponding `@@rpl_semi_sync_slave_enabled` values.
#
# Post-Conditions:
# * On connection `slave`.
# * None of the replication threads are running.
# * @@default_master_connection   = 'master1',
#   @@rpl_semi_sync_slave_enabled =   @mode2


# Start `master1`
SET @@SESSION.default_master_connection= 'master1';
SET @@GLOBAL.rpl_semi_sync_slave_enabled= @mode1;
--source include/start_slave.inc

# Start `master2`
SET @@SESSION.default_master_connection= 'master2';
SET @@GLOBAL.rpl_semi_sync_slave_enabled= @mode2;
--source include/start_slave.inc


# Verify `master2`
--connection master2
  CREATE TABLE t2 (a INT);
  DROP TABLE t2;
  SHOW STATUS LIKE 'Rpl_semi_sync_master_clients';
  --save_master_pos
--connection slave
--sync_with_master 0, master2

# Verify `master1`
--connection master1
  CREATE TABLE t1 (a INT);
  SHOW STATUS LIKE 'Rpl_semi_sync_master_clients';
  --save_master_pos
--connection slave
--sync_with_master 0, master1
SHOW STATUS LIKE 'Rpl_semi_sync_slave_send_ack';


# Stop `master2`
--source include/stop_slave.inc

# Verify `master1`
--connection master1
  DROP TABLE t1;
  SHOW STATUS LIKE 'Rpl_semi_sync_master_clients';
  --save_master_pos
--connection slave
--sync_with_master 0, master1
SHOW STATUS LIKE 'Rpl_semi_sync_slave_send_ack';

# Stop `master1`
SET @@SESSION.default_master_connection= 'master1';
--source include/stop_slave.inc
