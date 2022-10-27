# include/wait_for_binlog_checkpoint.inc
#
# SUMMARY
#
#    Wait until binlog checkpoint has been logged for current binlog file.
#    This is useful to avoid races with output difference for binlog
#    checkpoints, as these are logged asynchronously from the binlog
#    background thread.
#
# USAGE:
#
#    --source include/wait_for_binlog_checkpoint.inc

let $_wait_count= 300;

let $_found= 0;

while ($_wait_count)
{
  dec $_wait_count;
  let $_cur_binlog= query_get_value(SHOW MASTER STATUS, File, 1);
  let $_more= 1;
  let $_row= 1;
  while ($_more)
  {
    let $_event= query_get_value(SHOW BINLOG EVENTS IN "$_cur_binlog", Event_type, $_row);
    if ($_event == "No such row")
    {
      let $_more= 0;
    }
    if ($_event == "Binlog_checkpoint")
    {
      let $_info= query_get_value(SHOW BINLOG EVENTS IN "$_cur_binlog", Info, $_row);
      if (`SELECT INSTR("$_info", "$_cur_binlog") != 0`)
      {
        let $_more= 0;
        let $_wait_count= 0;
        let $_found= 1;
      }
    }
    inc $_row;
  }
  if ($_wait_count)
  {
    real_sleep 0.1;
  }
}

if (!$_found)
{
  eval SHOW BINLOG EVENTS IN "$_cur_binlog";
  --die ERROR: failed while waiting for binlog checkpoint $_cur_binlog
}
