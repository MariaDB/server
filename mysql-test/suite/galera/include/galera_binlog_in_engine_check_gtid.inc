# Check that node_1 and node_2 have binlogged the same last replicated
# transaction, in wsrep_gtid_domain_id (1). Both nodes must be synced.
# See also galera_binlog_in_engine_check_gtid_history.inc.
--connection node_1
--let $gtid_1 = `SELECT REGEXP_SUBSTR(@@gtid_binlog_pos, '(?<![0-9])1-[0-9]+-[0-9]+')`
--connection node_2
--let $wait_condition = SELECT REGEXP_SUBSTR(@@gtid_binlog_pos, '(?<![0-9])1-[0-9]+-[0-9]+') = '$gtid_1'
--source include/wait_condition.inc
--let $gtid_2 = `SELECT REGEXP_SUBSTR(@@gtid_binlog_pos, '(?<![0-9])1-[0-9]+-[0-9]+')`
if (!$gtid_1)
{
  --die No replicated transactions in gtid_binlog_pos of node_1
}
if ($gtid_1 != $gtid_2)
{
  --die gtid_binlog_pos domain 1 differs between the nodes: $gtid_1 vs $gtid_2
}
--echo # gtid_binlog_pos domain 1 matches on both nodes
