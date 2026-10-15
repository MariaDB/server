# Assert that purge has not freed the BLOB while a version-chain walk is parked
# at the dereference of its reference. Purging an update record is what would
# free it; the caller must have enabled and reset that monitor, the counter
# being cumulative over the lifetime of the server.
#
# The wait gives purge every chance to get there first, which is what makes the
# assertion fail rather than pass by accident; without the freeze it needs about
# a second. It cannot be a wait_condition, because a frozen purge_sys.view is
# precisely what stops every purge counter from moving.

--disable_query_log
let $purge_wait_rounds= 20;
while ($purge_wait_rounds)
{
  let $freed= `SELECT count FROM information_schema.innodb_metrics WHERE name = 'purge_upd_exist_or_extern_records'`;
  if ($freed)
  {
    let $purge_wait_rounds= 0;
  }
  if (!$freed)
  {
    real_sleep 0.1;
    dec $purge_wait_rounds;
  }
}
--enable_query_log
SELECT count AS upd_records_purged_while_parked FROM information_schema.innodb_metrics
WHERE name = 'purge_upd_exist_or_extern_records';
