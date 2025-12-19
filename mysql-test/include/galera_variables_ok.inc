--disable_query_log

let $_galera_variables_delta=$galera_variables_delta;
if (!$_galera_variables_delta) {
--let $galera_variables_delta=0
}

--let $galera_variables_expected=`SELECT 50 + $galera_variables_delta`

--let $galera_variables_count=`SELECT COUNT(*) FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES WHERE VARIABLE_NAME LIKE 'wsrep%'`

--if ($galera_variables_count != $galera_variables_expected) {
	--skip Galera number of variables has changed! ($galera_variables_count instead of $galera_variables_expected)
}

--enable_query_log
