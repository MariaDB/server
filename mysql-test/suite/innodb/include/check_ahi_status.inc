# Include file to execute a query with optional repetition and display
# values of r_ahi_stats on ANALYZE FORMAT=JSON
#
# Parameters:
#   $query - The query to execute
#   $repeat - Number of times to repeat the query before analyzing
#
# This include file will:
# 1. Execute the query $repeat times to warm up AHI (with logging disabled)
# 2. Execute ANALYZE FORMAT=JSON on the query and display the values

--disable_query_log
--disable_result_log

# Repeat the query to warm up AHI
let $i = $repeat;
while ($i > 0)
{
  eval $query;
  dec $i;
}

--enable_result_log
--enable_query_log

# Execute ANALYZE FORMAT=JSON once and capture output
--echo ANALYZE FORMAT=JSON $query
let $out=`ANALYZE FORMAT=JSON $query`;

# Parse JSON and extract AHI variables
--disable_query_log
evalp set @js='$out';
set @ahi_searches = COALESCE(json_extract(@js,'$**.r_engine_stats.r_ahi_stats.ahi_searches'), 0);
set @ahi_searches_btree = COALESCE(json_extract(@js,'$**.r_engine_stats.r_ahi_stats.ahi_searches_btree'), 0);
set @ahi_rows_added = COALESCE(json_extract(@js,'$**.r_engine_stats.r_ahi_stats.ahi_rows_added'), 0);
set @ahi_pages_added = COALESCE(json_extract(@js,'$**.r_engine_stats.r_ahi_stats.ahi_pages_added'), 0);
set @r_rows = json_extract(@js,'$**.table.r_rows');

# Display AHI variables
select @ahi_searches as ahi_searches,
       @ahi_searches_btree as ahi_searches_btree,
       @ahi_rows_added as ahi_rows_added,
       @ahi_pages_added as ahi_pages_added,
       @r_rows as r_rows;
--enable_query_log
