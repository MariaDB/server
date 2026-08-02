# Report worker threads started per parallel query, for the queries run since
# the last FLUSH STATUS. Both status variables are cumulative and
# --ps-protocol executes a statement more than once, so the ratio is what is
# stable across protocols. See parallel_query_worker_count.test.
--let $pqwpq_queries= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)
--let $pqwpq_workers= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_workers_started', Value, 1)
--disable_query_log
--eval SELECT $pqwpq_queries > 0 AS ran_in_the_workers, $pqwpq_workers / $pqwpq_queries AS workers_per_query
--enable_query_log
