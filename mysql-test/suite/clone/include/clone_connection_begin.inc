## Create connection to clone instance optionally restarting instance with
## monitoring process.

# These variables can to be set before sourcing this file. Currently we used
# to test both with and without monitoring process.
#
# 1. Need to restart mysqld with monitoring process
# --let inst_monitor = 1
#
# 2. Mysqld server instance number for clone
# --let clone_inst_number = <instance number> 1/2/3 ...
#     - Instances must be configured in .cnf
#     - SERVER_PORT_[n] ENV must be set to server PORT in .cnf
#
# 3. Mysqld user name for connecting to the instance.
# --let clone_user = <user_name>
#
# 4. Number of connections to be created to the instance
# --let clone_connections = <number of connections> 1/2/3 ...
# Connections have the name as clone_conn_<number>
# e.g. clone_conn_1, clone_conn_2, clone_conn_3 etc.
#
# This script should be used in pair with clone_connection_end.inc
# --source clone_connection_begin.inc
# ...
# --source clone_connection_end.inc

--disable_query_log
--let $clone_user = 'root'
--let $clone_connections = 1
--let $clone_port= \$SERVER_PORT_$clone_inst_number
--connect (clone_conn_1, 127.0.0.1, $clone_user,,test,$clone_port)
--connection default

# Get Server ID
--let $SERVER_ID= `SELECT @@server_id`

--connection default
--enable_query_log
