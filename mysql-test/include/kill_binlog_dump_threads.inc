# ==== Purpose ====
#
# Terminate all binlog dump threads on a master.
#
# This is sometimes useful, as normally such dump threads can hang
# around for some time before they notice that the slave has disconnected.
#
# Note that if there are active slave connections, they might try to
# reconnect as their dump threads are killed, which may not lead to the
# desired results.
#
#
# ==== Usage ====
#
# [--let $kill_timeout= NUMBER]
# --source include/stop_slavekill_binlog_dump_threads.inc
#
# Parameters:
#   $kill_timeout
#     Maximum number of seconds to wait for dump threads to disappear.


--let $include_filename= kill_binlog_dump_threads.inc
--source include/begin_include_file.inc

--disable_query_log

let $wait_counter= 300;
if ($kill_timeout)
{
  let $wait_counter= `SELECT $kill_timeout * 10`;
}

let $success= 0;
while ($wait_counter)
{
    dec $wait_counter;
    let $_tid= `SELECT id FROM information_schema.processlist WHERE command = 'Binlog Dump' LIMIT 1`;
    if ($_tid)
    {
        --error 0,ER_NO_SUCH_THREAD
        eval KILL QUERY $_tid;
    }
    if (!$_tid)
    {
        let $wait_counter= 0;
	let $success= 1;
    }
    if (!$success)
    {
        real_sleep 0.1;
    }
}
if (!$success)
{
    SHOW FULL PROCESSLIST;
    --die Timeout while waiting for binlog dump threads to disappear.
}
# This an attempt to get more info about a rare sporadic test failure where
# RESET MASTER still fails with ER_BINLOG_IN_USE after this has run.
--let $sanity_check= `SELECT COUNT(*) FROM information_schema.processlist WHERE command = 'Binlog Dump'`
if ($sanity_check > 0) {
  SHOW FULL PROCESSLIST;
  --echo ERROR: still $sanity_check dump thread(s) found!
  --die ERROR: still $sanity_check dump thread(s) found
}

--enable_query_log

--let $include_filename= kill_binlog_dump_threads.inc
--source include/end_include_file.inc
