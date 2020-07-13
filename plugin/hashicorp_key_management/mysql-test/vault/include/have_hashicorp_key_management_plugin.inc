if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'hashicorp_key_management' AND PLUGIN_STATUS='ACTIVE'`)
{
  --skip Test requires active hashicorp_key_management plugin
}
