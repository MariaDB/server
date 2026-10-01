#
# Shared body for the MDEV-40179 tests (sourced by MDEV-40179.test with
# log_bin=ON and MDEV-40179_nobinlog.test with log_bin=OFF).
#
# The bug (reproduced by the log_bin=ON variant):
#
# With log_bin=ON a transaction is committed via two-phase commit (the binary
# log is the second participant), so it passes through the InnoDB XA-prepare
# state. While a donor is held in BLOCK_COMMIT for a mariabackup backup, its
# parallel appliers (wsrep_slave_threads > 1) leave one or more such writesets
# prepared-but-not-yet-committed, and the snapshot captures them. On a freshly
# SST'd joiner nothing resolves these prepared transactions: binlog crash
# recovery does not run (the joiner has no in-use binlog to recover from), and
# the wsrep continuity-based commit is inactive because wsrep_emulate_bin_log
# is FALSE when log_bin is ON. The leftover prepared transactions then abort
# startup with "Found <N> prepared transactions!". Note this does not depend on
# the prepared set being non-contiguous - even a contiguous run aborts, because
# nothing commits or rolls it back.
#
# The log_bin=OFF variant is coverage only: with a single (InnoDB) read-write
# engine and no binary log, commits use one-phase commit, so transactions never
# enter the XA-prepared state and the snapshot has nothing in doubt. It simply
# verifies that mariabackup SST and reconvergence keep working with log_bin=OFF.
#
# To maximize parallel apply on the donor (and thus the chance of catching
# prepared transactions in the snapshot) each client thread writes to its own
# table: there are no certification conflicts between writers, so all of them
# apply concurrently. $writers client threads load on each of node_1 and node_2
# while node_3 is repeatedly stopped, has its data directory purged and is
# started again, forcing a full mariabackup SST on every rejoin. At the end the
# cluster must reconverge to three nodes and all three nodes must hold identical
# data (and, with log_bin, identical GTID positions).
#
# Parameters set by the including .test:
#   $restarts    - number of stop/purge/start cycles for node_3
#   $writers     - number of concurrent loader threads per node (each gets its
#                  own table to avoid certification conflicts)
#   $check_gtid  - 1 to also compare @@global.gtid_binlog_pos across nodes
#                  (only meaningful with log_bin), 0 otherwise
#

--source include/big_test.inc
--source include/galera_cluster.inc
--source include/have_innodb.inc
--source include/have_mariabackup.inc

--let $galera_connection_name = node_3
--let $galera_server_number = 3
--source include/galera_connect.inc

# Save original auto_increment_offset values so that MTR's post-check is
# happy after node_3 has been restarted multiple times.
--let $node_1=node_1
--let $node_2=node_2
--let $node_3=node_3
--source ../galera/include/auto_increment_offset_save.inc

# Total number of data tables: one per writer thread across both nodes.
--let $ntables = `SELECT 2 * $writers`

#
# Schema: t1_1 .. t1_$ntables hold the load (one table per writer thread),
# ctrl carries the stop flag for the loaders.
#
--connection node_1
--disable_query_log
CREATE TABLE ctrl (id INT PRIMARY KEY, stop INT) ENGINE=InnoDB;
INSERT INTO ctrl VALUES (1, 0);

--let $t = 1
while ($t <= $ntables)
{
  --eval CREATE TABLE t1_$t (pk BIGINT AUTO_INCREMENT PRIMARY KEY, val INT) ENGINE=InnoDB
  --inc $t
}

DELIMITER |;
CREATE PROCEDURE p_load(IN tname VARCHAR(64))
BEGIN
  DECLARE v_stop INT DEFAULT 0;
  DECLARE v_i INT;
  # Keep the loop alive across transient cluster errors (BF aborts,
  # certification failures, donor desync timeouts, ...).
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
  BEGIN
    ROLLBACK;
  END;
  SET @ins_sql = CONCAT('INSERT INTO ', tname, ' (pk, val) VALUES (DEFAULT, 1)');
  PREPARE ins FROM @ins_sql;
  WHILE v_stop = 0 DO
    START TRANSACTION;
    SET v_i = 0;
    WHILE v_i < 16 DO
      EXECUTE ins;
      SET v_i = v_i + 1;
    END WHILE;
    COMMIT;
    # Throttle slightly between transactions so that a freshly joined node can
    # catch up its replication queue instead of being starved by the load.
    DO SLEEP(0.01);
    SELECT stop INTO v_stop FROM ctrl WHERE id = 1;
  END WHILE;
  DEALLOCATE PREPARE ins;
END|
DELIMITER ;|
--enable_query_log

# Make sure the schema reached the other nodes before starting the load.
--connection node_2
--let $wait_condition = SELECT COUNT(*) = $ntables FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME LIKE 't1\_%';
--source include/wait_condition.inc
--connection node_3
--let $wait_condition = SELECT COUNT(*) = $ntables FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME LIKE 't1\_%';
--source include/wait_condition.inc

#
# Start the continuous load: $writers threads on node_1 (tables t1_1..t1_W)
# and $writers threads on node_2 (tables t1_(W+1)..t1_2W).
#
--disable_query_log
--let $w = 1
while ($w <= $writers)
{
  --connect (n1_load_$w, 127.0.0.1, root, , test, $NODE_MYPORT_1)
  --connect (n2_load_$w, 127.0.0.1, root, , test, $NODE_MYPORT_2)
  --inc $w
}
--enable_query_log

--let $w = 1
--let $n2 = $writers
while ($w <= $writers)
{
  --connection n1_load_$w
  --send_eval CALL p_load('t1_$w')
  --inc $n2
  --connection n2_load_$w
  --send_eval CALL p_load('t1_$n2')
  --inc $w
}

#
# While the load is running, repeatedly stop node_3, purge its data
# directory and start it again. An empty data directory forces a full
# mariabackup SST on every rejoin.
#
--disable_query_log
--let $i = $restarts
while ($i)
{
  --connection node_3
  --source include/shutdown_mysqld.inc
  --disable_query_log

  # Wait until node_3 has actually left the cluster.
  # (shutdown_mysqld.inc / wait_condition.inc / start_mysqld.inc /
  #  galera_wait_ready.inc each re-enable the query log, so re-disable it after
  #  every such include to keep the loop output out of the result file.)
  --connection node_1
  --let $wait_condition = SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
  --source include/wait_condition.inc
  --disable_query_log

  # Purge node_3's data directory.
  --remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.3/data/test
  --remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.3/data/mysql
  --remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.3/data/performance_schema
  --remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.3/data/mtr
  --remove_files_wildcard $MYSQLTEST_VARDIR/mysqld.3/data

  # Start node_3 again (rejoins via mariabackup SST).
  --connection node_3
  --let $restart_noprint = 2
  --source include/start_mysqld.inc
  --disable_query_log
  --source include/galera_wait_ready.inc
  --disable_query_log

  # Wait until the cluster is back to three nodes before the next cycle.
  --connection node_1
  --let $wait_condition = SELECT VARIABLE_VALUE = 3 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
  --source include/wait_condition.inc
  --disable_query_log

  --dec $i
}
--enable_query_log

#
# Make sure the whole cluster is healthy before stopping the load, so that
# any donor that desynced during SST has resynced and the loaders can read
# the stop flag without blocking.
#
--connection node_1
--source include/galera_wait_ready.inc
--connection node_2
--source include/galera_wait_ready.inc
--connection node_3
--source include/galera_wait_ready.inc

#
# Signal the loaders to stop and collect them.
#
--connection node_1
UPDATE ctrl SET stop = 1 WHERE id = 1;

--disable_query_log
--let $w = 1
while ($w <= $writers)
{
  --connection n1_load_$w
  --reap
  --connection n2_load_$w
  --reap
  --inc $w
}
--enable_query_log

#
# Build the aggregate count / checksum expressions over all data tables.
#
--let $count_expr = 0
--let $sum_expr = 0
--let $t = 1
while ($t <= $ntables)
{
  --let $count_expr = $count_expr + (SELECT COUNT(*) FROM t1_$t)
  --let $sum_expr = $sum_expr + (SELECT COALESCE(SUM(pk),0)+COALESCE(SUM(val),0) FROM t1_$t)
  --inc $t
}

#
# Verify reconvergence and data / GTID consistency across all nodes.
#
--connection node_1
SET SESSION wsrep_sync_wait = 15;
SELECT VARIABLE_VALUE AS wsrep_cluster_size FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';

# The load has stopped; issue one final transaction from node_1 (sync_wait is
# on, so node_1 first applies everything else). This makes node_1 the origin of
# the cluster's highest GTID, so the checks below can wait for node_2/node_3 to
# converge *up* to node_1's position instead of comparing a single snapshot:
# @@gtid_binlog_pos is a system variable, so reading it is not covered by
# wsrep_sync_wait and a plain read can otherwise sample a position before the
# node has finished applying (a race that grows with accumulated load, e.g.
# under --repeat).
--disable_query_log
UPDATE ctrl SET stop = 2 WHERE id = 1;
--enable_query_log

--let $expect_count = `SELECT $count_expr`
--let $expect_sum   = `SELECT $sum_expr`
if ($check_gtid)
{
  # Compare only the wsrep domain (wsrep_gtid_domain_id) of gtid_binlog_pos.
  # That is the part the whole cluster shares. Other domains in the position
  # are node-local and legitimately differ: e.g. CALL mtr.add_suppression()
  # below writes to the non-replicated 'mtr' database, which each node binlogs
  # under its own gtid_domain_id/server_id - so those entries accumulate
  # per-node across runs (visible under --repeat) and must not be compared.
  --let $wsrep_dom = `SELECT @@global.wsrep_gtid_domain_id`
  --let $expect_gtid = `SELECT REGEXP_SUBSTR(@@global.gtid_binlog_pos, '(?<![0-9])$wsrep_dom-[0-9]+-[0-9]+')`
}

--connection node_2
SET SESSION wsrep_sync_wait = 15;
--let $wait_condition = SELECT ($count_expr) = $expect_count
--source include/wait_condition.inc
--disable_query_log
if ($check_gtid)
{
  --let $wait_condition = SELECT REGEXP_SUBSTR(@@global.gtid_binlog_pos, '(?<![0-9])$wsrep_dom-[0-9]+-[0-9]+') = '$expect_gtid'
  --source include/wait_condition.inc
  --disable_query_log
  --eval SELECT $expect_count = ($count_expr) AS count_match, $expect_sum = ($sum_expr) AS checksum_match, '$expect_gtid' = REGEXP_SUBSTR(@@global.gtid_binlog_pos, '(?<![0-9])$wsrep_dom-[0-9]+-[0-9]+') AS gtid_match
}
if (!$check_gtid)
{
  --eval SELECT $expect_count = ($count_expr) AS count_match, $expect_sum = ($sum_expr) AS checksum_match
}
--enable_query_log

--connection node_3
SET SESSION wsrep_sync_wait = 15;
--let $wait_condition = SELECT ($count_expr) = $expect_count
--source include/wait_condition.inc
--disable_query_log
if ($check_gtid)
{
  --let $wait_condition = SELECT REGEXP_SUBSTR(@@global.gtid_binlog_pos, '(?<![0-9])$wsrep_dom-[0-9]+-[0-9]+') = '$expect_gtid'
  --source include/wait_condition.inc
  --disable_query_log
  --eval SELECT $expect_count = ($count_expr) AS count_match, $expect_sum = ($sum_expr) AS checksum_match, '$expect_gtid' = REGEXP_SUBSTR(@@global.gtid_binlog_pos, '(?<![0-9])$wsrep_dom-[0-9]+-[0-9]+') AS gtid_match
}
if (!$check_gtid)
{
  --eval SELECT $expect_count = ($count_expr) AS count_match, $expect_sum = ($sum_expr) AS checksum_match
}
--enable_query_log

#
# Cleanup.
#
--connection node_1
--disable_query_log
DROP PROCEDURE p_load;
--let $t = 1
while ($t <= $ntables)
{
  --eval DROP TABLE t1_$t
  --inc $t
}
DROP TABLE ctrl;

CALL mtr.add_suppression("WSREP: Failed to prepare for incremental state transfer");
CALL mtr.add_suppression("WSREP: Member .* requested state transfer");
CALL mtr.add_suppression("WSREP: .* returned an error: Not connected to Primary Component");
--enable_query_log

--connection node_2
--disable_query_log
CALL mtr.add_suppression("WSREP: Failed to prepare for incremental state transfer");
CALL mtr.add_suppression("WSREP: Member .* requested state transfer");
CALL mtr.add_suppression("WSREP: Did not find domain ID from SST script output");
CALL mtr.add_suppression("WSREP: Ignoring server id for non bootstrap node");
--enable_query_log

--connection node_3
--disable_query_log
CALL mtr.add_suppression("WSREP: Failed to prepare for incremental state transfer");
CALL mtr.add_suppression("WSREP: Member .* requested state transfer");
CALL mtr.add_suppression("InnoDB: Table \"mysql\"\\.\"innodb_index_stats\" not found");
CALL mtr.add_suppression("InnoDB: Table \"mysql\"\\.\"innodb_table_stats\" not found");
CALL mtr.add_suppression("Can't open and lock time zone table");
CALL mtr.add_suppression("Can't open and lock privilege tables");
CALL mtr.add_suppression("Table 'mysql\\.gtid_slave_pos' doesn't exist");
CALL mtr.add_suppression("Native table .* has the wrong structure");
CALL mtr.add_suppression("WSREP: Did not find domain ID from SST script output");
CALL mtr.add_suppression("WSREP: Ignoring server id for non bootstrap node");
CALL mtr.add_suppression("WSREP: Discovered discontinuity in recovered wsrep transaction XIDs");
CALL mtr.add_suppression("Rolled back orphan prepared wsrep transaction");
--enable_query_log

# Restore original auto_increment_offset values.
--source ../galera/include/auto_increment_offset_restore.inc

--source include/galera_end.inc
