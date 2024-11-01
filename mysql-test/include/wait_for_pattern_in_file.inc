# ==== Purpose ====
#
# Waits until pattern comes into log file or until a timeout is reached.
# This is a timeout wrapper for search_pattern_in_file.inc
#
#
# ==== Usage ====
#
# [--let $timeout= NUMBER in seconds]
# [--let $wait_for_pattern_count= Optional NUMBER of occurences to wait for]
# For other parameters, check search_pattern_in_file.inc

--let $wait_save_keep_include_silent=$keep_include_silent
--let $include_filename= wait_for_pattern_in_file.inc
--source include/begin_include_file.inc
--let $keep_include_silent= 1

let $_timeout= $timeout;
if (!$_timeout)
{
  let $_timeout= 10;
  if ($VALGRIND_TEST)
  {
    let $_timeout= 30;
  }
}

if (!$wait_for_pattern_count)
{
  # If we don't care about the number of occurrences of a pattern, use
  # NOT FOUND as the abort case
  let SEARCH_ABORT=NOT FOUND;
  let SEARCH_ABORT_IS_SUCCESS= 0;
}
if ($wait_for_pattern_count)
{
  # If we care about waiting for an exact number of occurrences, use that
  let SEARCH_ABORT=FOUND $wait_for_pattern_count;
  let SEARCH_ABORT_IS_SUCCESS= 1;
}

--echo # wait SA $SEARCH_ABORT SAIS $SEARCH_ABORT_IS_SUCCESS

let $_timeout_counter=`SELECT $_timeout * 10`;
let $_continue= 1;
disable_abort_on_error;
while ($_continue)
{
  source include/search_pattern_in_file.inc;
  if (!$errno)
  {
    # Found match
    let $_continue= 0;
  }
  if ($errno)
  {
    dec $_timeout_counter;
    if ($_timeout_counter == 1)
    {
      enable_abort_on_error;
    }
    if (!$_timeout_counter)
    {
        let $_continue= 0;
    }
  }
}
enable_abort_on_error;

--source include/end_include_file.inc
--let $keep_include_silent=$wait_save_keep_include_silent
