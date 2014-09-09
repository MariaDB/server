# galera_init.inc
# ===============
#
# Description
# -----------
# Set up a Galera cluster with $wsrep_cluster_size nodes.
#
# Parameters
# ----------
# $galera_cluster_size
#   Number of nodes in the cluster.
#

--source include/have_wsrep_provider.inc
--source include/have_wsrep_enabled.inc

--let $_galera_node= $galera_cluster_size

while ($_galera_node)
{
  --let $galera_connection_name= node_$_galera_node
  --let $galera_server_number= $_galera_node
  --source include/galera_connect.inc

  --dec $_galera_node
}

