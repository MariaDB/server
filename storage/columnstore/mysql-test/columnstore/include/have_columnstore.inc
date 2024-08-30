if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'columnstore' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires engine ColumnStore.
}

--disable_query_log
CREATE FUNCTION mcssystemready RETURNS INTEGER SONAME 'ha_columnstore.so';
--enable_query_log

if (`SELECT mcssystemready() = 0`)
{
  --skip Test requires ColumnStore to be running
}
