# Compare an aggregate query's answer run serially against the same query run by
# the parallel workers, and report whether the workers ran it.
#
# Parameters:
#   $pqa_query  the SELECT to compare, without a trailing semicolon
#
# For use where the answer does not depend on the order the rows were read, so
# that it can be compared as it stands rather than reduced to an
# order-independent fingerprint: an aggregate with no GROUP BY, which is one row,
# or a GROUP BY whose ORDER BY the manager applies to the finished groups. A
# GROUP BY with no ORDER BY is not one of those -- use
# include/parallel_query_fingerprint.inc for that.
#
# ran_in_the_workers is reported rather than required, because the shapes the
# gate refuses are tested here too -- for those the number is 0 and the answer
# must still be the right one.
--echo # $pqa_query
--disable_query_log
SET SESSION parallel_worker_threads= 0;
--enable_query_log
--eval $pqa_query
--disable_query_log
SET SESSION parallel_worker_threads= 4;
FLUSH STATUS;
--enable_query_log
--eval $pqa_query
--let $pqa_pq= query_get_value(SHOW SESSION STATUS LIKE 'Parallel_queries_executed', Value, 1)
--disable_query_log
--eval SELECT $pqa_pq > 0 AS ran_in_the_workers
--enable_query_log
