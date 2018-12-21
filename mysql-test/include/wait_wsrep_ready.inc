#
# If the wsrep plugin is loaded, wait until the wsrep provider becomes
# ready for use.
#

--disable_query_log
--disable_result_log

if (`SELECT COUNT(*)=1 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'wsrep' AND PLUGIN_STATUS='ACTIVE'`)
{
  if (`SELECT @@GLOBAL.WSREP_ON`)
  {
    --source include/galera_wait_ready.inc
  }
}

--enable_query_log
--enable_result_log
