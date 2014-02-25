# galera_end.inc
# ==============
#
# Description
# -----------
# Closes the connections opened via include/galera_init.inc
#
# Parameters
# ----------
# $galera_cluster_size
#   Number of nodes in the cluster.
#

--let $_galera_node= $galera_cluster_size

while ($_galera_node)
{
  if ($galera_debug)
  {
    --echo Disconnecting node_$_galera_node
  }
  --disconnect node_$_galera_node
  --dec $_galera_node
}

