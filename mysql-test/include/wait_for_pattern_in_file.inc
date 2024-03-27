# ==== Purpose ====
#
# Waits until pattern comes into log file or until a timeout is reached.
# This is a timeout wrapper for search_pattern_in_file.inc
#
#
# ==== Usage ====
#
# [--let $timeout= NUMBER in seconds]
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

let $_timeout_counter=`SELECT $_timeout * 10`;
let SEARCH_SILENT=1;

let $_continue= 1;
while ($_continue)
{
  source include/search_pattern_in_file.inc;
  if ($SEARCH_RESULT)
  {
    # Found match
    let $_continue= 0;
  }
  if (!$SEARCH_RESULT)
  {
    dec $_timeout_counter;
    if ($_timeout_counter == 1)
    {
      let $SEARCH_SILENT= 0;
    }
    if (!$_timeout_counter)
    {
        let $_continue= 0;
    }
  }
}

let SEARCH_SILENT=0;

--source include/end_include_file.inc
--let $keep_include_silent=$wait_save_keep_include_silent
