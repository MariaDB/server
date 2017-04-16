--exec suite/rocksdb_hotbackup/include/setup_replication_gtid.sh

let $slave_sync_timeout = 1800;
source include/wait_for_slave_to_sync_with_master.inc;
