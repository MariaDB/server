--source include/shutdown_mysqld.inc

# Expect the server to fail to come up with these options
--error 1
--exec $MYSQLD_CMD --plugin_load=$HA_ROCKSDB_SO $_mysqld_option 

# Restart the server with the default options
--source include/start_mysqld.inc
