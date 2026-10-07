# Make the buffer pool cold: shut down cleanly and restart.
# The test must have innodb_buffer_pool_dump_at_shutdown=OFF and
# innodb_buffer_pool_load_at_startup=OFF (see the .opt file).
SET GLOBAL innodb_fast_shutdown=0;
--let $restart_parameters=
--source include/restart_mysqld.inc
