# galera_cluster.inc
# ==================
#
# Description
# -----------
# Configure galera cluster with 2 nodes.
#

--let $galera_cluster_size = 2
--source include/galera_init.inc
