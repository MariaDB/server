--source include/have_innodb.inc
--source include/have_perfschema.inc
#
# The test verifies binlogging of XA transaction and state of prepared XA
# as far as binlog is concerned.
#
# The prepared XA transactions can be disconnected from the client,
# discovered from another connection and commited or rolled back
# later. They also survive the server restart.  The test runs two
# loops each consisting of prepared XA:s generation, their
# manipulation and a server restart followed with survived XA:s
# completion.
#
# Prepared XA can't get available to an external connection
# until connection that either leaves actively or is killed
# has completed a necessary part of its cleanup.
# Selecting from P_S.threads provides a method to learn that.
#
# Total number of connection each performing one insert into table
--let $conn_number=20
# Number of rollbacks and commits from either side of the server restart
--let $rollback_number=5
--let $commit_number=5
# Number of transactions that are terminated before server restarts
--let $term_number=`SELECT $rollback_number + $commit_number`
# Instead of disconnect make some connections killed when their
# transactions got prepared.
--let $killed_number=5
# make some connections disconnected by shutdown rather than actively
--let $server_disconn_number=5
--let $prepared_at_server_restart = `SELECT $conn_number - $term_number`
# number a "warmup" connection after server restart, they all commit
--let $post_restart_conn_number=10

# Counter to be used in GTID consistency check.
# It's incremented per each non-XA transaction commit.
# Local to this file variable to control one-phase commit loop
--let $one_phase_number = 5

--connection default

# Remove possibly preceeding binlogs and clear initialization time
# GTID executed info. In the following all transactions are counted
# to conduct verification at the end of the test.
if (`SELECT @@global.log_bin`)
{
  RESET MASTER;
}

# Disconected and follower threads need synchronization
CREATE VIEW v_processlist  as SELECT * FROM performance_schema.threads where type = 'FOREGROUND';

--eval call mtr.add_suppression("Found $prepared_at_server_restart prepared XA transactions")

CREATE TABLE t (a INT) ENGINE=innodb;

# MDEV-515 takes X-lock on the table for the first insert.
# So concurrent insert won't happen on the table
INSERT INTO t VALUES(100);

# Counter is incremented at the end of post restart to
# reflect number of loops done in correctness computation.
--let $restart_number = 0
--let $how_to_restart=restart_mysqld.inc
--source suite/binlog/include/binlog_xa_prepared_do_and_restart.inc

--let $how_to_restart=kill_and_restart_mysqld.inc
--source suite/binlog/include/binlog_xa_prepared_do_and_restart.inc

--connection default

# Few xs that commit in one phase, not subject to the server restart
# nor reconnect.
# This piece of test is related to mysqlbinlog recovery examine below.
--let $k = 0
while ($k < $one_phase_number)
{
    --eval XA START   'one_phase_trx_$k'
    --eval INSERT INTO t SET a=$k
    --eval XA END     'one_phase_trx_$k'
    --eval XA COMMIT  'one_phase_trx_$k' ONE PHASE

    --inc $k
}

SELECT SUM(a) FROM t;
DROP TABLE t;
DROP VIEW v_processlist;

let $outfile= $MYSQLTEST_VARDIR/tmp/mysqlbinlog.sql;
if (`SELECT @@global.log_bin`)
{
  # Recording proper samples of binlogged prepared XA:s
  --source include/show_binlog_events.inc
  --exec $MYSQL_BINLOG -R --to-last-log master-bin.000001 > $outfile
}

--echo All transactions must be completed, to empty-list the following:
XA RECOVER;

if (`SELECT @@global.log_bin`)
{
  --exec $MYSQL test < $outfile
  --remove_file $outfile
  XA RECOVER;
}
