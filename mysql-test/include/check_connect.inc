# Check if connect engine is loaded and enabled

if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'CONNECT' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires connect engine
}
