# Helper for main.pqkmr's interval-count section. Reports the plan's "type"
# for $q and whether the parallel read path was taken.
#
# The plan type has to be shown next to the verdict: "parallel: no" only proves
# the engine declined if the optimizer actually asked for a range scan. Once an
# IN list covers enough of the table the optimizer switches to type=ALL, and
# then the parallel *full* scan fires - a YES that says nothing about the
# interval limit.
--disable_query_log
--replace_column 5 # 6 # 7 # 9 #
eval explain $q;

eval set session debug_dbug="+d:t:O,$trace";
eval $q;
set session debug_dbug="";
--enable_query_log
--exec grep -q "ha_parallel_get_next_row" $trace && echo "  parallel: YES" || echo "  parallel: no"
--exec rm -f $trace
