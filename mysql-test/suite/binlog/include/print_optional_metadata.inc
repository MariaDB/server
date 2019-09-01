# Auxaliary file for printing optional metadata in table_map_log_event
# Usage :
# --let $binlog_file=
# [--let $stop_position]
# [--let $print_primary_key]
# --source extra/binlog_tests/print_optional_metadata.inc

--let $output_file= $MYSQLTEST_VARDIR/tmp/mysqlbinlog.output

--let $_stop_position_opt=
if ($stop_position)
{
  --let $_stop_position_opt=--stop-position=$stop_position
}

--exec $MYSQL_BINLOG -F --print-table-metadata $_stop_position_opt $binlog_file > $output_file


--let SEARCH_PATTERN= # (?:Columns\(| {8}).*
--let SEARCH_FILE= $output_file
--let SEARCH_OUTPUT=matches
--let SEARCH_TYPE="_gm_"
--source include/search_pattern_in_file.inc

if ($print_primary_key)
{
  --let SEARCH_PATTERN= # Primary Key
  --source include/search_pattern_in_file.inc
}
--remove_file $output_file
--let $stop_position=
--let $_stop_position_opt=


