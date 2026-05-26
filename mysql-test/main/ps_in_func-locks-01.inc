# Run $routine, which parks itself on the user lock 'l1' after having read
# t1, and check that ALTER TABLE t1 can complete meanwhile, i.e. that the
# metadata lock taken on t1 was released at the end of the statement which
# read it, rather than kept until $routine returns.
#
# Parameters:
#   $routine  procedure to call
#   $comment  table comment ALTER TABLE sets, to wait for

connect (c2,localhost,root,,);
connect (c3,localhost,root,,);

connection c2;
--disable_ps2_protocol
SELECT GET_LOCK('l1', 0);
--disable_ps2_protocol

connection default;
--send_eval CALL $routine()

connection c3;
--let $wait_condition= SELECT COUNT(*) FROM information_schema.processlist WHERE state='User lock'
--source include/wait_condition.inc
--send_eval ALTER TABLE t1 COMMENT '$comment'

connection c2;
--let $wait_condition= SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='test' AND table_name='t1' AND table_comment='$comment'
--source include/wait_condition.inc
--echo # ALTER TABLE t1 completed while $routine is still running
--disable_ps2_protocol
SELECT RELEASE_LOCK('l1');
--enable_ps2_protocol

connection default;
--reap
--disable_ps2_protocol
SELECT RELEASE_LOCK('l1');
--enable_ps2_protocol
connection c3;
--reap

connection default;
DELETE FROM t2;
disconnect c2;
disconnect c3;
