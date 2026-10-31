# include file for test files that can be run with and without the
# disconnect_watcher, see disconnect_watcher.combinations.
# Sets $disconnect_watcher, 0 or 1.
# Skips the test if the server was started with the value that does not
# fit the combination, for example with --mysqld=--skip-disconnect-watcher.
--source include/disconnect_watcher_value.inc
if (`select find_in_set('on', '$MTR_COMBINATIONS') and not $disconnect_watcher`)
{
  --skip Test requires: 'disconnect_watcher=ON' for the combination 'on'
}
if (`select find_in_set('off', '$MTR_COMBINATIONS') and $disconnect_watcher`)
{
  --skip Test requires: 'disconnect_watcher=OFF' for the combination 'off'
}
