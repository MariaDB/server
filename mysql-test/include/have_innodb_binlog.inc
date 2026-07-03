--source include/have_innodb.inc

# Use GTID for --sync_slave_with_master, since non-GTID is not available
# with engine-in-binlog.
--enable_sync_gtid
