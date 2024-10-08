# ==== Purpose ====
#
# Issues STOP SLAVE IO_THREAD on the current connection. Then waits
# until the IO thread has stopped, or until a timeout is reached.
#
# This will fail if the slave IO thread has an error. If you expect an
# error in the IO thread, use include/wait_for_slave_io_error.inc
# instead.
#
# Please use this instead of 'STOP SLAVE IO_THREAD', to reduce the
# risk of races in test cases.
#
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# --source include/stop_slave_io.inc
#
# Parameters:
#   $slave_timeout
#     See include/wait_for_slave_param.inc
#
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= stop_slave_io.inc
--source include/begin_include_file.inc


if (!$rpl_debug)
{
  --disable_query_log
}

let $_enable_warnings=0;
if ($rpl_allow_error) {
  if ($ENABLED_WARNINGS) {
    let $_enable_warnings=1;
    disable_warnings;
  }
}
STOP SLAVE IO_THREAD;
if ($_enable_warnings) {
  enable_warnings;
}
--source include/wait_for_slave_io_to_stop.inc


--let $include_filename= stop_slave_io.inc
--source include/end_include_file.inc
