# Compare how many rows "SELECT a FROM t1 $pqlc_lim" delivers serially and in the
# workers. Counted through a non-merged derived table, so the figure is the whole
# query's row count; FOUND_ROWS() would count rows the executor processed, which
# with OFFSET includes the skipped ones. See parallel_query_limit.test.
--disable_query_log
SET SESSION parallel_worker_threads= 0;
--eval SELECT COUNT(*) INTO @serial FROM (SELECT a FROM t1 $pqlc_lim) d
SET SESSION parallel_worker_threads= 4;
FLUSH STATUS;
--eval SELECT COUNT(*) INTO @par FROM (SELECT a FROM t1 $pqlc_lim) d
--let $pqlc_pq= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)
--enable_query_log
--echo # $pqlc_lim
--disable_query_log
--eval SELECT @serial AS rows_serial, @par AS rows_parallel, @serial = @par AS same, $pqlc_pq > 0 AS ran_in_the_workers
--enable_query_log
