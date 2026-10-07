# Run ANALYZE FORMAT=JSON on $ra_query and extract the read-ahead statistics:
#   @pages_accessed  pages the statement accessed
#   @pages_prefetch  pages supplied by read-ahead (0 if none)
# $ra_query must not contain single quotes (it is quoted into the JSON).
--disable_query_log
LET $analyze_output= `ANALYZE FORMAT=JSON $ra_query`;
evalp SET @js='$analyze_output';
SET @js= JSON_EXTRACT(@js, '$.query_block.nested_loop[0].table.r_engine_stats');
SET @pages_accessed= CAST(JSON_VALUE(@js,'$.pages_accessed') AS INT);
SET @pages_prefetch= COALESCE(CAST(JSON_VALUE(@js,'$.pages_prefetch_read_count') AS INT),0);
if (`SELECT @pages_accessed IS NULL`)
{
  --die ANALYZE FORMAT=JSON did not return r_engine_stats for $ra_query
}
--enable_query_log
