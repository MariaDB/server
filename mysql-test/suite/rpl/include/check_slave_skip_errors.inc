# ==== Purpose ====
#
# MDEV-38624: Verify that ER_CONNECTION_KILLED (1927) cannot be added to
# --slave-skip-errors, even when explicitly specified or when using "all".
#
# Called from tests that configure --slave-skip-errors with 1927 or "all".
# Checks that 1927 is absent from slave_skip_errors and that the expected
# warning appears in the slave error log.

--connection slave
call mtr.add_suppression("Slave: ER_CONNECTION_KILLED .* is not allowed in --slave-skip-errors");

--let $skip_errors= query_get_value(SHOW VARIABLES LIKE 'slave_skip_errors', Value, 1)
if (`SELECT '$skip_errors' LIKE '%1927%'`)
{
  --die MDEV-38624: Error 1927 must not be added to slave_skip_errors
}

--let $assert_file= $MYSQLTEST_VARDIR/log/mysqld.2.err
--let $assert_select= ER_CONNECTION_KILLED .* is not allowed in --slave-skip-errors
--let $assert_count= 1
--let $assert_text= Warning about ER_CONNECTION_KILLED should appear in error log
--source include/assert_grep.inc
