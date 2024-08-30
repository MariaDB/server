--disable_query_log
if (`SELECT from_unixtime((1<<31)+24*3600) is null`) {
  let $sys_time_max_replace= 2038-01-19 03:14:07.999999;
  let $sys_timestamp_max=2147483647;
}
if (`SELECT from_unixtime((1<<31)+24*3600) is not null`) {
  let $sys_time_max_replace= 2106-02-07 06:28:15.999999;
  let $sys_timestamp_max=4294967295;
}
let $sys_time_max= TIMESTAMP'$sys_time_max_replace';
let $sys_datatype_max= $sys_time_max;
--enable_query_log
