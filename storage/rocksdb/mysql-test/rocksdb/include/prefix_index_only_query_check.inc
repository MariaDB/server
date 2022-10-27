#
# A helper include file for prefix index index-only query tests
#
# Parameters:
# $prefix_index_check_title - title of the test
# $prefix_index_check_query - test query
# $prefix_index_check_read_avoided_delta - expected change of
#   'rocksdb_covered_secondary_key_lookups' status variable
#   value after running the query

--let $show_count_statement = show status like 'rocksdb_covered_secondary_key_lookups'

--echo # $prefix_index_check_title
--let $base_count = query_get_value($show_count_statement, Value, 1)

--eval $prefix_index_check_query

--let $count = query_get_value($show_count_statement, Value, 1)
--let $assert_text= $prefix_index_check_title: $prefix_index_check_read_avoided_delta rocksdb_covered_secondary_key_lookups
--let $assert_cond= $count - $base_count = $prefix_index_check_read_avoided_delta
--source include/assert.inc
