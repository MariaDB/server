# Waits for N members in the cluster
#
# Usage:
# --let $members=1
# --source wsrep_wait_membership.inc
#

let $wait_condition = SELECT VARIABLE_VALUE = $members FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';

--source include/wsrep_wait_condition.inc
