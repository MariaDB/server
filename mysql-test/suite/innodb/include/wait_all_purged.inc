# Wait for everything to be purged.
# The user should have set innodb_purge_rseg_truncate_frequency=1.

--disable_query_log
if (!$wait_all_purged)
{
  SET GLOBAL innodb_max_purge_lag_wait= 0;
}
if ($wait_all_purged)
{
  eval SET GLOBAL innodb_max_purge_lag_wait= $wait_all_purged;
}
--enable_query_log

--replace_regex /.*History list length ([0-9]+).*/\1/
let $remaining= `SHOW ENGINE INNODB STATUS`;
echo $remaining transactions not purged;
