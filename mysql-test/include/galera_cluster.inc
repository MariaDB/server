# galera_cluster.inc
# ==================
#
# Description
# -----------
# Configure galera cluster with $galera_cluster_size (default: 2) nodes.
#

if (!$galera_cluster_size)
{
#  --die ERROR IN TEST: $galera_cluster_size variable must be set
  --let $galera_cluster_size = 2
}

--source include/galera_init.inc
--source include/have_innodb.inc
--source include/galera_wait_ready.inc


--let $_galera_node= $galera_cluster_size

while ($_galera_node != 1)
{
  --connection node_$_galera_node
  --source include/galera_wait_ready.inc
  --source include/have_innodb.inc

  --dec $_galera_node
}

--connection node_1
