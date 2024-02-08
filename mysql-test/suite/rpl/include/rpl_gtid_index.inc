# Include file for main test rpl.rpl_gtid_index.
# Test GTID indexes with given parameters.
#
# Parameters:
#   $NUM_POS             Number of GTIDs/binlog positions to create
#   $NUM_DOMAIN          Number of different domains to use
#   $NUM_SERVER          Number of different server_id to use
#   $NUM_SLAVE_CONNECTS  How many GTID slave connect positions to test
#   $RND_SEED            Random seed


--echo *** Testing $NUM_POS GTIDs with $NUM_SLAVE_CONNECTS test connects

--connection master
DELETE FROM t1 WHERE a >= 1000;
# Rotate binlogs to make new GTID index settings take effect.
FLUSH NO_WRITE_TO_BINLOG BINARY LOGS;

# Prepare some random values, but deterministic between test runs.
CREATE TABLE rand_data(idx INT PRIMARY KEY, domain_id INT, server_id INT)
  ENGINE=InnoDB;
INSERT INTO rand_data(idx, domain_id, server_id) VALUES (0, 0, 1);
eval
INSERT INTO rand_data(idx, domain_id, server_id)
SELECT seq,
       @tmp:=floor($NUM_DOMAIN*POW(rand($RND_SEED),2)),
       100 + $NUM_SERVER*@tmp + floor($NUM_SERVER*rand($RND_SEED))
  FROM seq_1_to_$NUM_POS;
# Let's check that the test data is deterministic.
# If this changes due to some server changes, it's fine, the .result can just
# be updated. But we want it to be identical between test runs on same code,
# to facilitate debugging test failures.
SELECT COUNT(*), SUM(domain_id), SUM(server_id) FROM rand_data;

# Create some data for the binlog (and GTID index), recording the correct
# binlog positions and GTIDs.
CREATE TABLE gtid_data(
  idx INT PRIMARY KEY,
  gtid VARCHAR(44),
  gtid_pos VARCHAR(255),
  file VARCHAR(100),
  pos INT,
  row_count INT,
  KEY(file, pos)) ENGINE=InnoDB;
--let $gtid= `SELECT @@last_gtid`

--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc

--connection master
SET @orig_domain_id= @@gtid_domain_id;
SET @orig_server_id= @@server_id;
--let $i= 0
--let $rotate_point= `SELECT floor($NUM_POS/2)`
--let $base_count= `SELECT COUNT(*) FROM t1`
--disable_query_log
while ($i < $NUM_POS) {
  --let $file= query_get_value(SHOW MASTER STATUS, File, 1)
  --let $pos= query_get_value(SHOW MASTER STATUS, Position, 1)
  --let $gtid_pos= `SELECT @@gtid_binlog_pos`
  --let $row_count= `SELECT $base_count + $i`
  eval SET gtid_domain_id= (SELECT domain_id FROM rand_data WHERE idx=$i+1);
  eval SET server_id= (SELECT server_id FROM rand_data WHERE idx=$i+1);
  BEGIN;
  eval INSERT INTO gtid_data(idx, gtid, gtid_pos, file, pos, row_count)
       VALUES ($i, '$gtid', '$gtid_pos', '$file', $pos, $row_count);
  eval INSERT INTO t1 VALUES ($i + 1000, 0);
  COMMIT;
--let $gtid= `SELECT @@last_gtid`
  inc $i;
  if ($i==$rotate_point) {
    FLUSH NO_WRITE_TO_BINLOG BINARY LOGS;
  }
}
--enable_query_log
SET gtid_domain_id= @orig_domain_id;
SET server_id= @orig_server_id;

SELECT COUNT(*) FROM gtid_data;

# Test that BINLOG_GTID_POS returns correct positions for every GTID position.
--echo *** The result should be empty, otherwise some result is wrong:
SELECT idx, gtid_pos, BINLOG_GTID_POS(file, pos)
  FROM gtid_data
 WHERE NOT gtid_eq(CONVERT(gtid_pos USING utf8),BINLOG_GTID_POS(file, pos)) 
 ORDER BY idx;

# Prepare to rewind the slave to this point to test again on same binlog.
--connection slave
SET @orig_pos= @@GLOBAL.gtid_slave_pos;
SET @orig_t1_limit= (SELECT MAX(a) FROM t1);

--echo *** Now connect the slave to each position in turn, and test that
--echo *** the right amount of data is replicated at each point.
--let $old_silent= $keep_include_silent
--let $keep_include_silent= 1
--let $i= 0
--disable_query_log
while ($i < $NUM_POS) {
  --connection master
  --let $gtid_pos= `SELECT gtid_pos FROM gtid_data WHERE idx=$i`
  --let $master_count= `SELECT row_count FROM gtid_data WHERE idx=$i`
  --connection slave
  --disable_result_log
  eval START SLAVE UNTIL master_gtid_pos='$gtid_pos';
  --enable_result_log
  --let $res= `SELECT MASTER_GTID_WAIT('$gtid_pos')`
  if ($res != 0) {
    --die "FAIL: MASTER_GTID_WAIT($gtid_pos) returned $res, should have been 0"
  }
  --source include/wait_for_slave_to_stop.inc
  --let $slave_count = `SELECT COUNT(*) FROM t1`
  if ($master_count != $slave_count) {
    SELECT * FROM gtid_data ORDER BY file, pos;
    SELECT * FROM t1 ORDER BY a;
    --die "Not all rows replicated. $master_count on master but $slave_count on slave."
  }
  --let $i= `SELECT $i + ceil($NUM_POS / $NUM_SLAVE_CONNECTS)`
}
--enable_query_log

--echo *** Test slave connecting to some GTID positions where the position in
--echo *** the master's binlog is different between the different domains.
--echo *** Revind the slave and test on the same binlog data from the master as before.
--connection slave
SET sql_log_bin= 0;
TRUNCATE gtid_data;
DELETE FROM t1 WHERE a > @orig_t1_limit;
SET sql_log_bin= 1;
SET GLOBAL gtid_slave_pos= @orig_pos;

--let $i= 0
--disable_query_log
while ($i <= $NUM_DOMAIN) {
  # Build a GTID position from GTIDs that are picked at different locations
  # in the gtid_data table for each domain.
  --connection master
  let $until_pos=`
    SELECT GROUP_CONCAT(gtid SEPARATOR ',')
      FROM gtid_data
     WHERE idx IN (
       SELECT MAX(gtid_data.idx) AS pick
	 FROM gtid_data
	 INNER JOIN rand_data ON (rand_data.idx = gtid_data.idx)
	WHERE gtid_data.idx*$NUM_DOMAIN <= (domain_id + $i)*$NUM_POS
	GROUP BY domain_id
      )`;
  --connection slave
  --disable_result_log
  eval START SLAVE UNTIL master_gtid_pos='$until_pos';
  --enable_result_log
  --let $res= `SELECT MASTER_GTID_WAIT('$until_pos')`
  if ($res != 0) {
    --die "FAIL: MASTER_GTID_WAIT($until_pos) returned $res, should have been 0"
  }
  --source include/wait_for_slave_to_stop.inc

  inc $i;
}
--enable_query_log
--let $keep_include_silent= $old_silent

# Check that everything was replicated (nothing skipped).
# We have one less row on the slave since the last UNTIL is the one before
# the master inserted the last row.
--connection master
--let $master_count= `SELECT COUNT(*)-1 FROM t1`
--connection slave
--let $slave_count= `SELECT COUNT(*) FROM t1`
if ($master_count != $slave_count) {
  SELECT * FROM gtid_data ORDER BY file, pos;
  SELECT * FROM t1 ORDER BY a;
  --die "Not all rows replicated. $master_count on master but $slave_count on slave."
}

--connection master
DROP TABLE gtid_data, rand_data;
--source include/save_master_gtid.inc

--connection slave
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

--connection master
