# MARIAROCKS_NOT_YET: replication doesn't work yet:
#if (`select count(*) = 0 from information_schema.tables where engine='rocksdb' and table_name='slave_gtid_info'`)
#{
#  --skip Test requires default engine RocksDB
#}

--disable_query_log
# Table statistics can vary depending on when the memtables are flushed, so
# flush them at the beginning of the test to ensure the test runs consistently.
set global rocksdb_force_flush_memtable_now = true;
--enable_query_log
