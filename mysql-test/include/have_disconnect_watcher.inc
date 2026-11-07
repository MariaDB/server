# include file: skip the test unless the disconnect_watcher is ON.
# Sets $disconnect_watcher, see disconnect_watcher_value.inc.

--source include/disconnect_watcher_value.inc
if (!$disconnect_watcher)
{
  --skip Test requires: 'disconnect_watcher=ON'
}
