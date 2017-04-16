if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'rocksdb' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires engine RocksDB.
}

--disable_query_log
# Table statistics can vary depending on when the memtables are flushed, so
# flush them at the beginning of the test to ensure the test runs consistently.
set global rocksdb_force_flush_memtable_now = true;
--enable_query_log
