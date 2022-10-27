# To be used in a test which requires server to be compiled with wsrep support
# (-DWITH_WSREP=ON) and wsrep plugin is ACTIVE.

if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'wsrep' AND PLUGIN_STATUS='ACTIVE'`)
{
  --skip Test requires wsrep plugin.
}

