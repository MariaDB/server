## Create connection to clone instance optionally restarting instance with
## monitoring process.

# These variables can to be set before sourcing this file. Currently we used
# to test both with and without monitoring process.
#
# 1. Number of connections to be dropped from the instance
# --let clone_connections = <number of connections> 1/2/3 ...
# Connections have the name as clone_conn_<number>
# e.g. clone_conn_1, clone_conn_2, clone_conn_3 etc.
#
# 2. This script should be used in pair with clone_connection_begin.inc
# --source clone_connection_begin.inc
# ...
# --source clone_connection_end.inc

if (!$clone_user) {
  --let $clone_user = 'root'
}

if (!$clone_connections) {
  --let $clone_connections = 1
}

# A. Drop the connections
--connection clone_conn_1

# Get Server ID
--let $SERVER_ID= `SELECT @@server_id`

--let $conn_number = 1

echo $SERVER_ID;
# If not the default instance, keep first connection.
if ($SERVER_ID != 1) {
 --let $conn_nummber =2

 --echo Uninstall clone plugin on recipient server
 UNINSTALL PLUGIN clone;
}

while ($conn_number <= $clone_connections)
{
  --let $conn_name = clone_conn_$conn_number
  --disconnect $conn_name
  --inc $conn_number
}

# Now disconnect the skipped connection
if ($SERVER_ID != 1) {
  --disconnect clone_conn_1
}

# Switch to default connection
--connection default
