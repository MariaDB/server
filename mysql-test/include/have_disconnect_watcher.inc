# include file: skip the test unless the disconnect_watcher is ON.
# Sets $disconnect_watcher, see disconnect_watcher_value.inc.

--source include/disconnect_watcher_value.inc
if (!$disconnect_watcher)
{
  --skip Test requires: 'disconnect_watcher=ON'
}

# These tests drop a client in the middle of a statement on purpose, and
# expect the reason in the server log. No other test should have it.
--disable_query_log
call mtr.add_suppression("Client disconnected");
--enable_query_log
