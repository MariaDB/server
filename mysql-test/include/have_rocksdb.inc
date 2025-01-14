if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'ROCKSDB' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires MyRocks engine
}
