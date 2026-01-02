if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'connect' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires Connect engine
}
