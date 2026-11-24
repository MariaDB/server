# include/wait_condition_with_debug_and_kill.inc
#
# SUMMARY
#
#    Waits until the passed statement returns true, or the operation
#    times out.  If the operation times out, the additional error
#    statement will be executed, the server is killed to leave a core
#    for post-mortem analysis, and the test is aborted.
#
#    The abort is essential: the node is gone once we return, so any
#    statement the caller would execute next runs against a dead server
#    and fails in an unrelated and non-deterministic way, hiding the
#    timeout that actually caused the failure.
#
# USAGE
#
#    let $wait_condition=
#      SELECT c = 3 FROM t;
#    let $wait_condition_on_error_output= select count(*) from t;
#    [let $explicit_default_wait_timeout= N] # to override the default reset
#    --source include/wait_condition_with_debug_and_kill.inc
#
#   OR
#
#    let $wait_timeout= 60; # Override default 30 seconds with 60.
#    let $wait_condition=
#      SELECT c = 3 FROM t;
#    let $wait_condition_on_error_output= select count(*) from t;
#    --source include/wait_condition_with_debug_and_kill.inc
#    --echo Executed the test condition $wait_condition_reps times
#
#
# EXAMPLE
#    events_bugs.test, events_time_zone.test
#

--source include/wait_condition_with_debug.inc
if (!$success)
{
  --source include/kill_galera.inc
  --die Timeout in wait_condition_with_debug_and_kill.inc for $wait_condition
}
