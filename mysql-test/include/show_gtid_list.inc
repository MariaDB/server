# ==== Purpose ====
#
# Extract Gtid_list info from SHOW BINLOG EVENTS output masking
# non-deterministic fields.
#
# ==== Usage ====
#
# [--let $binlog_file=filename
#
if ($binlog_file)
{
  --let $_in_binlog_file=in '$binlog_file'
}
--replace_column 2 # 5 #
--eval show binlog events $_in_binlog_file limit 1,1
