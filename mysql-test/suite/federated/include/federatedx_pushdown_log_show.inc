# Stop capturing and show the captured statements matching
# $log_pattern, and $log_pattern2 if set (LIKE patterns).
connection slave;
SET global general_log=0;
--let $log_cond= argument LIKE '$log_pattern'
if ($log_pattern2)
{
  --let $log_cond= $log_cond OR argument LIKE '$log_pattern2'
}
eval SELECT argument FROM mysql.general_log
  WHERE $log_cond;
--let $log_pattern2=
connection master;
