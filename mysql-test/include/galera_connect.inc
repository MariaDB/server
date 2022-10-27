# galera_connect.inc
# ==================
#
# Description
# -----------
# Open a connection to the specified server number ($galera_server_number).
# The connection itself would be identified by $galera_connection_name.
#
# Parameters
# ----------
# $galera_connection_name
#   Name of the resulting connection.
#
# $galera_server_number
#   Sequence number of the node in the galera cluster.
#
# $galera_debug
#   Print debug information.
#

if (!$galera_connection_name)
{
  --die ERROR IN TEST: $galera_connection_name must be set before sourcing include/galera_connect.inc
}

if (!$galera_server_number)
{
  --die ERROR IN TEST: $galera_server_number must be set before sourcing include/galera_connect.inc
}

--let $_galera_port= \$NODE_MYPORT_$galera_server_number
if (!$_galera_port)
{
  --echo Bug in test case: '\$NODE_MYPORT_$galera_server_number' not initialized. Check the test's .cfg file.
  --die Not all NODE_MYPORT_* environment variables are setup correctly.
}

if ($galera_debug)
{
--disable_query_log
  --echo connect($galera_connection_name,127.0.0.1,root,,test,$_galera_port,)
--enable_query_log
}

# Temporal solution to avoid concurrent IST MDEV-7178
--sleep 1

# Open a connection
--disable_query_log
--connect($galera_connection_name,127.0.0.1,root,,test,$_galera_port,)
--enable_query_log
