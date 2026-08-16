# Helper for main.pqidx: report the plan and whether the parallel read path was
# taken for $q.
#
# "type"/"key"/"Extra" are shown because that is what separates the cases this
# file is about: a covering secondary index scan is type=index + Using index,
# while losing the covering property drops it to type=ALL on the clustered
# index - which is parallelized today and so would otherwise look like success.
--echo # ---- $q
--disable_query_log
--replace_column 9 #
eval explain $q;

eval set session debug_dbug="+d:t:O,$trace";
--disable_result_log
eval $q;
--enable_result_log
set session debug_dbug="";
--enable_query_log
--exec grep -q "ha_parallel_get_next_row" $trace && echo "     parallel: YES" || echo "     parallel: no"
--exec rm -f $trace
