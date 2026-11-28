# galera_cluster.inc
# ==================
#
# Description
# -----------
# Configure galera cluster with $galera_cluster_size (default: 2) nodes.
#
# Parameters
# ----------
# $galera_wait_ready_timeout (optional)
#   Passed through to every node's include/galera_wait_ready.inc wait (see
#   that file). Useful for a cluster that is slow to reach READY, e.g. on a
#   loaded or slow CI builder, or with more than a couple of nodes.
#

if (!$galera_cluster_size)
{
#  --die ERROR IN TEST: $galera_cluster_size variable must be set
  --let $galera_cluster_size = 2
}

# Preserve a caller-supplied $galera_wait_ready_timeout across all of this
# file's internal waits. galera_wait_ready.inc resets it after each use, so
# without this, only the first node's wait would see it (MDEV-41925).
--let $_galera_cluster_wait_ready_timeout = $galera_wait_ready_timeout

--source include/galera_init.inc
--source include/have_innodb.inc
--let $galera_wait_ready_timeout = $_galera_cluster_wait_ready_timeout
--source include/galera_wait_ready.inc


--let $_galera_node= $galera_cluster_size

while ($_galera_node != 1)
{
  --connection node_$_galera_node
  --let $galera_wait_ready_timeout = $_galera_cluster_wait_ready_timeout
  --source include/galera_wait_ready.inc
  --source include/have_innodb.inc

  --dec $_galera_node
}

--connection node_1
