if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'rocksdb' AND support IN ('DEFAULT')`)
{
  --skip Test requires default engine RocksDB
}

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

