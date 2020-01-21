--source include/have_debug.inc
--disable_query_log
if ($MTR_COMBINATION_VERS)
{
  set @mtr_vers_saved_dbug= @debug_dbug;
  set @mtr_vers_saved_system_versioning_alter_history= @@system_versioning_alter_history;
  set debug_dbug= "d,sysvers_force";
  set @@system_versioning_alter_history= keep;
}
--enable_query_log
