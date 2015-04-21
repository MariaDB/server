# Include this script only after using shutdown_mysqld.inc
# where $_expect_file_name was initialized.
# Write file to make mysql-test-run.pl start up the server again

if ($galera_wsrep_start_position != '') {
	--echo Using --wsrep-start-position when starting mysqld ...
	--exec echo "restart:$start_mysqld_params --wsrep-start-position=$galera_wsrep_start_position" > $_expect_file_name
	--let $galera_wsrep_start_position = 0
}

if ($galera_wsrep_start_position == '') {
	--exec echo "restart:$start_mysqld_params" > $_expect_file_name
}

# Turn on reconnect
--enable_reconnect

# Call script that will poll the server waiting for it to be back online again
--source include/wait_until_connected_again.inc

# Turn off reconnect again
--disable_reconnect
