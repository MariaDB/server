--source include/have_debug.inc
--disable_query_log
if ($MTR_COMBINATION_VERS)
{
  set debug_dbug= @mtr_vers_saved_dbug;
  set @@system_versioning_alter_history= @mtr_vers_saved_system_versioning_alter_history;
}
--enable_query_log
