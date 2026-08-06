# Report whether every parallel scan since the last FLUSH STATUS aggregated in
# the workers. Both status variables are cumulative and --ps-protocol executes a
# statement more than once, so the ratio is what is stable across protocols:
# 1 means every scan pre-aggregated, 0 means none did. NULL when nothing ran in
# the workers at all. See parallel_query_preaggregate.test.
--let $pqpr_queries= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)
--let $pqpr_preagg= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_partial_aggregations', Value, 1)
--disable_query_log
--eval SELECT $pqpr_preagg / NULLIF($pqpr_queries,0) AS preaggregated_per_scan
--enable_query_log
