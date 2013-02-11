if ($binlog_start)
{
  --let $_binlog_start=$binlog_start
}
if (!$binlog_start)
{
  --let $_binlog_start=246
}
if ($binlog_file)
{
  --let $_in_binlog_file=in '$binlog_file'
}
--replace_result $_binlog_start <binlog_start> $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--replace_column 2 # 5 #
--replace_regex /\/\* xid=.* \*\//\/* XID *\// /table_id: [0-9]+/table_id: #/ /file_id=[0-9]+/file_id=#/
--eval show binlog events $_in_binlog_file from $_binlog_start
