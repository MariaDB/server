# Crash mysqld hard and wait until it's restarted

--source include/have_debug_sync.inc
--source include/not_embedded.inc

# Write file to make mysql-test-run.pl expect crash and restart
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

# Setup the mysqld to crash at shutdown
SET debug_dbug="d,crash_shutdown";
--error 2013
shutdown;

# Turn on reconnect
--enable_reconnect

# Call script that will poll the server waiting for it to be back online again
--source include/wait_until_connected_again.inc
