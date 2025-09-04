# These variables can to be set before sourcing this file. Currently we used
# to test both with and without monitoring process.
#
# 2. Number of connections to be dropped from the instance
# --let clone_connections = <number of connections> 1/2/3 ...
# Connections have the name as clone_conn_<number>
# e.g. clone_conn_1, clone_conn_2, clone_conn_3 etc.
#
# 3. This script should be used in pair with clone_connection_begin.inc
# --source clone_connection_begin.inc
# ...
# --source clone_connection_end.inc
--disable_query_log
--let $clone_user = 'root'
--let $clone_connections = 1

# A. Drop the connections
--connection clone_conn_1
# In case server is restarted, we need to reconnect
--enable_reconnect
--source include/wait_until_connected_again.inc
# Get Server ID
--disconnect clone_conn_1
# Switch to default connection
--connection default
--enable_query_log
