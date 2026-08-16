# Helper for main.pqorder: report the plan's "type"/"Extra" and whether the
# parallel read path was taken for $q.
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
