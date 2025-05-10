# include/wait_for_binlog_event.inc
#
# SUMMARY
#
#    Waits until SHOW BINLOG EVENTS has returned in last event a specified substring.
#
# USAGE
#
#    let $wait_binlog_file= 'master-bin.000001', defaults to the current log
#    let $wait_binlog_event= DROP;
#       OR
#    let $wait_binlog_event= ;pos=4  # as Rotate event signature in the SBE Info

#    --source include/wait_for_binlog_event.inc

let $_loop_count= 300;
let $_last_event= ;
let $_event_pos= 1;
if (!$wait_binlog_file)
{
  let $wait_binlog_file=query_get_value(SHOW MASTER STATUS, File, 1);
}
while (`SELECT INSTR("$_last_event","$wait_binlog_event") = 0`)
{
  dec $_loop_count;
  if (!$_loop_count)
  {
    --source include/show_rpl_debug_info.inc
    --die ERROR: failed while waiting for $wait_binlog_event in binlog 
  }
  real_sleep 0.1;
  let $_event= query_get_value(SHOW BINLOG EVENTS IN '$wait_binlog_file', Info, $_event_pos);
  let $_last_event= $_event;
  while ($_event != "No such row")
  {
    inc $_event_pos;
    let $_last_event= $_event;
    let $_event= query_get_value(SHOW BINLOG EVENTS IN '$wait_binlog_file', Info, $_event_pos);
  }
}
