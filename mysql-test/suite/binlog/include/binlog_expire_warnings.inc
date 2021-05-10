# ==== Purpose ====
#
# Test one scenario using a combination of --expire-logs-days and
# --binlog-expire-logs-seconds. Verify that there are/aren't warnings
# as expected, and verify that the values of the global variables are
# as expected.
#
# ==== Usage ====
#
# --let $ofile = FILE
# --let $options = SERVER_OPTIONS
# --let $days = VALUE
# --let $seconds = VALUE
# --let $expect_binlog_off_days_and_seconds_warning = [0|1]
# --let $expect_seconds = VALUE
# --let $option_invoke_order = [days_then_seconds|seconds_then_days]
# --let $expect_days = VALUE
# --source include/binlog_expire_warnings.inc
#
# Parameters:
#
#  $ofile
#    Temporary file to use for the error log.
#
#  $options
#    Any additional options passed to mysqld during server start.
#
#  $days
#    The value to set for --expire-logs-days
#
#  $seconds
#    The value to set for --binlog-expire-logs-seconds
#
#  $option_invoke_order
#    In which order options --expire-logs-days and
#    --binlog-expire-logs-seconds should be set.
#    values are: days_then_seconds, seconds_then_days
#
#  $expect_binlog_off_days_and_seconds_warning
#    If zero, assert that there is no warning due to using
#    --expire_logs_days or --binlog_expire_logs_seconds binlog disabled.
#    If nonzero, assert that there is a warning due to using
#    --expire_logs_days or --binlog_expire_logs_seconds binlog disabled.
#
#  $expect_days
#    Assert that @@global.expire_logs_days has this value.
#
#  $expect_seconds
#    Assert that @@global.binlog_expire_logs_seconds has this value.


--let $restart_parameters = --log-error=$ofile $options
if ($option_invoke_order == 'days_then_seconds')
{
  if ($days != '') {
    --let $restart_parameters = $restart_parameters --expire_logs_days=$days
  }
  if ($seconds != '') {
    --let $restart_parameters = $restart_parameters --binlog-expire-logs-seconds=$seconds
  }
}

if ($option_invoke_order == 'seconds_then_days')
{
  if ($seconds != '') {
    --let $restart_parameters = $restart_parameters --binlog-expire-logs-seconds=$seconds
  }
  if ($days != '') {
    --let $restart_parameters = $restart_parameters --expire_logs_days=$days
  }
}

--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--source include/restart_mysqld.inc

# For all the assert_grep.inc
--let $extra_debug_eval = LOAD_FILE("$ofile")

if (!$expect_binlog_off_days_and_seconds_warning) {
  --let $assert_text = There shall be no binlog_off+seconds warning
  --let $assert_count = 0
}

if ($expect_binlog_off_days_and_seconds_warning) {
  --let $assert_text = There shall be a binlog_off+seconds warning
  --let $assert_count = 1
}

--let $assert_file = $ofile
--let $assert_select = You need to use --log-bin to make --expire-logs-days or --binlog-expire-logs-seconds work.
--let $assert_only_after = Shutdown complete
--source include/assert_grep.inc

# For all the assert.inc
--let $extra_debug_eval = CONCAT("days: ", @@global.expire_logs_days, " seconds: ", @@global.binlog_expire_logs_seconds)

--let $assert_text = binlog_expire_logs_seconds shall be $expect_seconds
--let $assert_cond = @@global.binlog_expire_logs_seconds = $expect_seconds
--source include/assert.inc

--let $assert_text = expire_logs_days shall be $expect_days
--let $assert_cond = @@global.expire_logs_days = $expect_days
--source include/assert.inc
