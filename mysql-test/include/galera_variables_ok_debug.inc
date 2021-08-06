--disable_query_log

--let $galera_variables_ok = `SELECT COUNT(*) = 50 FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES WHERE VARIABLE_NAME LIKE 'wsrep%'`

--if (!$galera_variables_ok) {
	--skip Galera number of variables has changed!
}

--enable_query_log
