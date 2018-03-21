#
# If the wsrep plugin is loaded, wait until the wsrep provider becomes
# ready for use.
#

--disable_query_log
--disable_result_log

#
# if node is not ready, SELECT @@GLOBAL.WSREP_ON below does not
# succeed. However, we can force it to succeed by running the SELECT
# with dirty reads configuration. So we prepare for that
#
SET @my_dirty_reads= @@wsrep_dirty_reads;
SET wsrep_dirty_reads = ON;

if (`SELECT COUNT(*)=1 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'wsrep' AND PLUGIN_STATUS='ACTIVE'`)
{
  if (`SELECT @@GLOBAL.WSREP_ON`)
  {
    --source include/galera_wait_ready.inc
  }
}

# ..and returning original dirty read selection here
#
# NOTE: we should allow SELECT <variable> even when dirty reads is not set
#
#SET wsrep_dirty_reads = @my_dirty_reads;
SET wsrep_dirty_reads = OFF;

--enable_query_log
--enable_result_log
