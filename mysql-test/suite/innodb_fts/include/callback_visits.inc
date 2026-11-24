# Run $query with the debug keyword fts_count_callback_records set, and
# assert that it took $visits_min to $visits_max internal callback visits.
--let $_visits_before= query_get_value(SHOW GLOBAL STATUS LIKE 'Innodb_row_search_callback_records', Value, 1)
eval $query;
--let $_visits_after= query_get_value(SHOW GLOBAL STATUS LIKE 'Innodb_row_search_callback_records', Value, 1)
--let $assert_cond= $_visits_after - $_visits_before BETWEEN $visits_min AND $visits_max
--source include/assert.inc
