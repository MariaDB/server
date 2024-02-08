# Include this script after a shutdown to wait until the pid file,
# stored in $pid_file, has disappered.

#--echo $pid_file

--disable_result_log
--disable_query_log
# Wait one minute
let $counter= 600;
while ($counter)
{
--error 0,1
--file_exists $pid_file
  if (!$errno)
  {
    dec $counter;
    --real_sleep 0.1
  }
  if ($errno)
  {
    let $counter= 0;
  }
}
if (!$errno)
{
  --die Pid file "$pid_file" failed to disappear
}

--enable_query_log
--enable_result_log
