## Create connection to clone instance optionally restarting instance with
## monitoring process.

# These variables can to be set before sourcing this file. Currently we used
# to test both with and without monitoring process.
#
# 1. Mysqld server instance number for clone
# --let clone_inst_number = <instance number> 1/2/3 ...
#     - Instances must be configured in .cnf
#     - SERVER_PORT_[n] ENV must be set to server PORT in .cnf
#
# 2. Mysqld user name for connecting to the instance.
# --let clone_user = <user_name>
#
# 3. Number of connections to be created to the instance
# --let clone_connections = <number of connections> 1/2/3 ...
# Connections have the name as clone_conn_<number>
# e.g. clone_conn_1, clone_conn_2, clone_conn_3 etc.
#
# This script should be used in pair with clone_connection_end.inc
# --source clone_connection_begin.inc
# ...
# --source clone_connection_end.inc

if (!$clone_user) {
  --let $clone_user = 'root'
}

if (!$clone_connections) {
  --let $clone_connections = 1
}

if ($clone_inst_number) {
  --let $clone_port= \$SERVER_PORT_$clone_inst_number
  --connect (clone_conn_1, 127.0.0.1, $clone_user,,test,$clone_port)
}

if (!$clone_inst_number) {
  --connection default
}

# Get Server ID
--let $SERVER_ID= `SELECT @@server_id`

echo $SERVER_ID;
if ($clone_inst_number) {
  if ($SERVER_ID == 1) {
    --disconnect clone_conn_1
    --connection default
  }
}

if ($SERVER_ID != 1) {
  --echo Install clone plugin on recipient server
  --replace_result $CLONE_PLUGIN CLONE_PLUGIN
  --eval INSTALL PLUGIN clone SONAME '$CLONE_PLUGIN'
}

# B. Create the connections
--let $conn_nummber = 1

# For non default server, we have already created the first connection
if ($SERVER_ID != 1) {
  --let $conn_nummber = 2
}

while ($conn_nummber <= $clone_connections)
{
  --let $conn_name = clone_conn_$conn_nummber

  if ($clone_inst_number) {
    --let $clone_port= \$SERVER_PORT_$clone_inst_number
    --connect ($conn_name, 127.0.0.1, $clone_user,,test,$clone_port)
  }

  # Connect to the default instance if instance number is not provided.
  if (!$clone_inst_number) {
    --connect ($conn_name, localhost, $clone_user,,test)
  }
  --inc $conn_nummber
}
--connection default
