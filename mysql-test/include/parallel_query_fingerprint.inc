# Compare a query's result multiset run serially against the same query run by
# parallel workers, and check that the workers really did the scanning.
#
# Parameters:
#   $pq_query  the SELECT to compare, without a trailing semicolon
#   $pq_cols   its output column names, comma separated
#
# The result set cannot be written to a table to be compared afterwards:
# writing from a SELECT is a locking read, which makes the engine decline the
# parallel scan (see main/parallel_query_fallback), so both sides would run
# serially and the comparison would prove nothing. Instead $pq_query goes inside
# a non-mergeable derived table -- its inner select runs in the workers while
# the outer aggregate runs in this thread -- and is reduced to an
# order-independent fingerprint, because a parallel result arrives in
# batch-completion order.
#
# Parallel execution is asserted from Parallel_queries_executed, which counts
# queries the workers ran. The optimizer trace only reports which table the
# optimizer picked, and the engine can still decline after that.
#
# The caller must have set optimizer_switch='derived_merge=off'.

--let $pq_fp= COUNT(*) AS n, SUM(CRC32(CONCAT_WS('/',$pq_cols))) AS crc_sum, BIT_XOR(CRC32(CONCAT_WS('/',$pq_cols))) AS crc_xor

--echo # serial fingerprint
SET SESSION parallel_worker_threads= 0;
FLUSH STATUS;
--eval SELECT $pq_fp FROM ($pq_query) d
--let $pq_ser= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)

--echo # parallel fingerprint, must be identical to the serial one above
SET SESSION parallel_worker_threads= 4;
FLUSH STATUS;
--eval SELECT $pq_fp FROM ($pq_query) d
--let $pq_par= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)

--echo # the workers ran the query above, and did not run the serial one (expect 1)
--disable_query_log
--eval SELECT $pq_ser = 0 AND $pq_par > 0 AS ran_in_the_workers
--enable_query_log
