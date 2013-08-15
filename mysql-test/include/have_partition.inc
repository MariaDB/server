if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'partition' AND PLUGIN_STATUS='ACTIVE'`)
{
  --skip Test requires partitioning
}
