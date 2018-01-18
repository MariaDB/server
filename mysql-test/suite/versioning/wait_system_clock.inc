#
# windows has a rather low-resolution system clock
# wait until the event from the past will actually be in the past
#
if (`select @@version_compile_os in ("win32","win64","windows")`)
{
  let $_past=`select max(row_start) from t1`;
  --let $wait_condition=select TIMESTAMP'$_past' < sysdate(6)
  --source include/wait_condition.inc
}
