--disable_query_log

--let $galera_have_debug_sync = `SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_debug_sync_waiters'`

--if (!$galera_have_debug_sync) {
	--skip Test requires Galera debug library with debug_sync functionality
}

--enable_query_log
