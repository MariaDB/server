# callee of rpl_rocksdb_xa_recover.test
# requires t1,t2 as defined in the caller.

--let $at=_prepare
--let $finally_expected=$xa in the list
--let $init_val=`SELECT MAX(a) from t1 as t_1`
--eval XA START '$xa'
--eval INSERT INTO t1 SET a=$init_val + 1
--eval INSERT INTO t2 SET a=$init_val + 1
--eval XA END '$xa'
--eval SET SESSION DEBUG_DBUG="d,simulate_crash_$when$at"

--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--error 2013 # CR_SERVER_LOST
--eval XA PREPARE '$xa'
--source include/wait_until_disconnected.inc
--let $rpl_server_number = 1
--source include/rpl_reconnect.inc
--echo "*** $finally_expected"
XA RECOVER;
--eval XA ROLLBACK '$xa'
--eval SELECT MAX(a) - $init_val  as zero FROM t1
--eval SELECT MAX(a) - $init_val  as zero FROM t2


--let $at=_commit_or_rollback
--let $finally_expected=empty list (rolled back)
--eval XA START '$xa'
--eval INSERT INTO t1 SET a=$init_val + 2
--eval INSERT INTO t2 SET a=$init_val + 2
--eval XA END '$xa'
--eval XA PREPARE '$xa'

--eval SET SESSION DEBUG_DBUG="d,simulate_crash_$when$at"
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--error 2013 # CR_SERVER_LOST
--eval XA ROLLBACK '$xa'
--source include/wait_until_disconnected.inc
--let $rpl_server_number = 1
--source include/rpl_reconnect.inc

--echo "*** $finally_expected"
XA RECOVER;
--eval SELECT MAX(a) - $init_val  as zero FROM t1
--eval SELECT MAX(a) - $init_val  as zero FROM t2


--let $at=_commit_or_rollback
--let $finally_expected=empty list (committed away)
--eval XA START '$xa'
--eval INSERT INTO t1 SET a=$init_val + 3
--eval INSERT INTO t2 SET a=$init_val + 3
--eval XA END '$xa'
--eval XA PREPARE '$xa'

--eval SET SESSION DEBUG_DBUG="d,simulate_crash_$when$at"
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--error 2013 # CR_SERVER_LOST
--eval XA COMMIT '$xa'
--source include/wait_until_disconnected.inc
--let $rpl_server_number = 1
--source include/rpl_reconnect.inc

--echo "*** $finally_expected"
XA RECOVER;
--eval SELECT MAX(a) - $init_val as three FROM t1
--eval SELECT MAX(a) - $init_val as three FROM t2
