# include/wait_condition_with_debug.inc
#
# SUMMARY
#
#    Waits until the passed statement returns true, or the operation
#    times out.  If the operation times out, the additional error
#    statement will be executed.
#
# USAGE
#
#    let $wait_condition=
#      SELECT c = 3 FROM t;
#    let $wait_condition_on_error_output= select count(*) from t;
#    [let $explicit_default_wait_timeout= N] # to override the default reset
#    --source include/wait_condition_with_debug.inc
#
#   OR
#
#    let $wait_timeout= 60; # Override default 30 seconds with 60.
#    let $wait_condition=
#      SELECT c = 3 FROM t;
#    let $wait_condition_on_error_output= select count(*) from t;
#    --source include/wait_condition_with_debug.inc
#    --echo Executed the test condition $wait_condition_reps times
#
#
# EXAMPLE
#    events_bugs.test, events_time_zone.test
#

let $wait_counter= 300;
if ($wait_timeout)
{
  let $wait_counter= `SELECT $wait_timeout * 10`;
}
# Reset $wait_timeout so that its value won't be used on subsequent
# calls, and default will be used instead.
if ($explicit_default_wait_timeout)
{
  --let $wait_timeout= $explicit_default_wait_timeout
}
if (!$explicit_default_wait_timeout)
{
  --let $wait_timeout= 0
}

# Keep track of how many times the wait condition is tested
# This is used by some tests (e.g., main.status)
let $wait_condition_reps= 0;
while ($wait_counter)
{
    --error 0,ER_NO_SUCH_TABLE,ER_LOCK_WAIT_TIMEOUT,ER_UNKNOWN_COM_ERROR,ER_LOCK_DEADLOCK
    let $success= `$wait_condition`;
    inc $wait_condition_reps;
    if ($success)
    {
        let $wait_counter= 0;
    }
    if (!$success)
    {
        real_sleep 0.1;
        dec $wait_counter;
    }
}
if (!$success)
{
  echo Timeout in wait_condition.inc for $wait_condition;
  --eval $wait_condition_on_error_output
}
