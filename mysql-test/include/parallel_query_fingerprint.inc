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
# The caller must have set optimizer_switch='derived_merge=off'.

--let $pq_fp= COUNT(*) AS n, SUM(CRC32(CONCAT_WS('/',$pq_cols))) AS crc_sum, BIT_XOR(CRC32(CONCAT_WS('/',$pq_cols))) AS crc_xor

--echo # serial fingerprint
SET SESSION parallel_worker_threads= 0;
FLUSH STATUS;
--eval SELECT $pq_fp FROM ($pq_query) d
--let $pq_h_ser= query_get_value(SHOW SESSION STATUS LIKE 'Handler_read_rnd_next', Value, 1)

--echo # parallel fingerprint, must be identical to the serial one above
SET SESSION parallel_worker_threads= 4;
FLUSH STATUS;
--eval SELECT $pq_fp FROM ($pq_query) d
--let $pq_h_par= query_get_value(SHOW SESSION STATUS LIKE 'Handler_read_rnd_next', Value, 1)

# both runs read the same materialized rows; only the serial one also scans the
# driving table, so the serial thread's rnd_next count must be the higher one
--echo # the workers did the scanning, not this thread (expect 1)
--disable_query_log
--eval SELECT $pq_h_ser > $pq_h_par AS manager_scan_avoided
--enable_query_log
