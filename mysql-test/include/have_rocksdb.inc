if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'rocksdb'`)
{
  --skip Requires rocksdb
}