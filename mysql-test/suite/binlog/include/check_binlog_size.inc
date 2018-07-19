# This file runs the query and checks
# whether the size of binlog is increased or not
# If size is changed it issue die command
# Parameters
# $sql_query = query to run

#Only last row of show binlog events matter
--let $tmp= 0
--let $counter= 1
while ($tmp != "No such row")
{
  --let $initial_binlog_size= $tmp
  --let $tmp= query_get_value(show binary logs, File_size, $counter)
  --inc $counter
}

--eval  $sql_query

--let $tmp= 0
--let $counter= 1
while ($tmp != "No such row")
{
  --let $current_binlog_size= $tmp
  --let $tmp= query_get_value(show binary logs, File_size, $counter)
  --inc $counter
}

if ($initial_binlog_size != $current_binlog_size)
{
  die "Binlog size changed";
}
