# ===========================================================================
# MDEV-28213: the master must not validate the connecting slave's GTID
# position in domains that the slave has asked to filter away with
# IGNORE_DOMAIN_IDS or DO_DOMAIN_IDS.
#
# The combinations of master binlog state and slave GTID position are set up
# directly with RESET MASTER / SET GLOBAL gtid_binlog_state / SET GLOBAL
# gtid_slave_pos.  That way each case is exact and no binlog purging (nor
# waiting for it) is needed.
#
# The interesting property to check in each case is not only that the slave
# can connect, but also which GTIDs the master then sends in the filtered
# domain: it must never re-send a GTID that the slave already has, as that
# would move gtid_slave_pos backwards.
#
# Sourced by:
#   suite/rpl/t/rpl_gtid_ignored_domain_ids.test             (binlog in files)
#   suite/binlog_in_engine/rpl_gtid_ignored_domain_ids.test  (binlog in engine)
# ===========================================================================

--connection server_2
# Each test first shows what the master answers without a domain filter.
call mtr.add_suppression("Got fatal error 1236 from master when reading data from binary log");

--connection server_1
CREATE TABLE t1 (a INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE t2 (a INT PRIMARY KEY) ENGINE=InnoDB;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc


--echo #
--echo # Test 1: the ignored domain is in the master's binlog state, but the
--echo # slave has no position at all in it.
--echo #

--connection server_2
--source include/stop_slave.inc
--let $binlog_state= 1-1-100,2-1-50
--let $slave_pos= 1-1-100
--source suite/rpl/t/rpl_gtid_ignored_domain_ids_setup.inc

--connection server_2
--echo # Without a filter the master rejects the connection: it cannot know
--echo # where in domain 2 the slave wants to start.
CHANGE MASTER TO DO_DOMAIN_IDS=(), IGNORE_DOMAIN_IDS=(),
                 MASTER_USE_GTID=slave_pos;
START SLAVE;
--let $slave_io_errno= 1236
--source include/wait_for_slave_io_error.inc
--source include/stop_slave_sql.inc

--echo # With IGNORE_DOMAIN_IDS=(2) the same connection is accepted.
CHANGE MASTER TO IGNORE_DOMAIN_IDS=(2);
--source include/start_slave.inc

--connection server_1
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (101);
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (51);
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (102);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
SELECT * FROM t1 ORDER BY a;
--echo # Domain 2 was thrown away by the slave, so t2 is still empty.
SELECT * FROM t2 ORDER BY a;
--echo # ... but the slave position in the ignored domain did follow along.
SELECT @@GLOBAL.gtid_slave_pos RLIKE '(^|,)2-1-51(,|$)' AS domain_2_pos_reached;


--echo #
--echo # Test 2: the slave drops the filter again and keeps replicating the
--echo # previously ignored domain.  No manual repair of gtid_slave_pos is
--echo # needed, because ignoring a domain still advances the position in it.
--echo #

--connection server_2
--source include/stop_slave.inc
CHANGE MASTER TO IGNORE_DOMAIN_IDS=();
--source include/start_slave.inc

--connection server_1
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (52);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--echo # Only the row from after the filter was dropped; 51 stays skipped.
SELECT * FROM t2 ORDER BY a;


--echo #
--echo # Test 3: DO_DOMAIN_IDS, which filters away every domain not listed.
--echo #

--connection server_2
--source include/stop_slave.inc
--let $binlog_state= 1-1-200,2-1-100
--let $slave_pos= 1-1-200
--source suite/rpl/t/rpl_gtid_ignored_domain_ids_setup.inc

--connection server_2
CHANGE MASTER TO DO_DOMAIN_IDS=(1), IGNORE_DOMAIN_IDS=();
--source include/start_slave.inc

--connection server_1
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (101);
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (201);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;


--echo #
--echo # Test 4: every domain is filtered away, and the slave has no GTID
--echo # position at all.  The master then simply starts from the most recent
--echo # binlog state record.
--echo #

--connection server_2
--source include/stop_slave.inc
--let $binlog_state= 1-1-300,2-1-200
--let $slave_pos=
--source suite/rpl/t/rpl_gtid_ignored_domain_ids_setup.inc

--connection server_2
CHANGE MASTER TO DO_DOMAIN_IDS=(), IGNORE_DOMAIN_IDS=(1,2);
--source include/start_slave.inc

--connection server_1
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (301);
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (201);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--echo # Nothing was applied, but the position followed the master.
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;
SELECT @@GLOBAL.gtid_slave_pos RLIKE '(^|,)1-1-301(,|$)' AS domain_1_pos_reached;
SELECT @@GLOBAL.gtid_slave_pos RLIKE '(^|,)2-1-201(,|$)' AS domain_2_pos_reached;


--echo #
--echo # Test 5: the slave does have a position in the ignored domain, and it
--echo # is ahead of the master's binlog state (the master lost the events in
--echo # a purge).  The master must not send those GTIDs again, or it would
--echo # move the slave position backwards.
--echo #

--connection server_2
--source include/stop_slave.inc
--let $binlog_state= 1-1-400,2-1-300
--let $slave_pos= 1-1-400,2-1-350
--source suite/rpl/t/rpl_gtid_ignored_domain_ids_setup.inc

--connection server_2
CHANGE MASTER TO DO_DOMAIN_IDS=(), IGNORE_DOMAIN_IDS=(2);
--source include/start_slave.inc

--connection server_1
--echo # These are all behind the slave position in domain 2.
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (301);
INSERT INTO t2 VALUES (302);
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (401);
--source include/save_master_gtid.inc

--connection server_2
--let $master_pos= 1-1-401
--source include/sync_with_master_gtid.inc
--echo # The domain 2 position did not move backwards to 2-1-302.
SELECT @@GLOBAL.gtid_slave_pos RLIKE '(^|,)2-1-350(,|$)' AS domain_2_pos_not_moved_back;
SELECT * FROM t1 ORDER BY a;

--connection server_1
--echo # Once the master gets past the slave position, the domain resumes.
SET SESSION gtid_domain_id= 2;
SET SESSION gtid_seq_no= 351;
INSERT INTO t2 VALUES (351);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
SELECT @@GLOBAL.gtid_slave_pos RLIKE '(^|,)2-1-351(,|$)' AS domain_2_pos_resumed;


--echo #
--echo # Test 6: START SLAVE UNTIL, with an ignored domain in the master's
--echo # binlog state that the slave has no position in.
--echo #

--connection server_2
--source include/stop_slave.inc
--let $binlog_state= 1-1-500,2-1-400
--let $slave_pos= 1-1-500
--source suite/rpl/t/rpl_gtid_ignored_domain_ids_setup.inc

--connection server_1
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (501);
SET SESSION gtid_domain_id= 2;
INSERT INTO t2 VALUES (401);
SET SESSION gtid_domain_id= 1;
INSERT INTO t1 VALUES (502);
INSERT INTO t1 VALUES (503);
--source include/save_master_gtid.inc

--connection server_2
CHANGE MASTER TO IGNORE_DOMAIN_IDS=(2);
START SLAVE UNTIL master_gtid_pos= '1-1-502';
--source include/wait_for_slave_to_stop.inc
--echo # Stopped after 1-1-502, and nothing from the ignored domain applied.
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;

--echo # Continuing without UNTIL picks up the rest.
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
SELECT * FROM t1 ORDER BY a;


--echo #
--echo # Cleanup
--echo #

--connection server_2
--source include/stop_slave.inc
CHANGE MASTER TO DO_DOMAIN_IDS=(), IGNORE_DOMAIN_IDS=();
--source include/start_slave.inc

--connection server_1
SET SESSION gtid_domain_id= 0;
DROP TABLE t1, t2;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
