source include/check_windows_admin.inc;

# The test uses return code from sc.exe utility, which are as follows
let $ERROR_SERVICE_DOES_NOT_EXIST= 1060;
let $ERROR_SERVICE_CANNOT_ACCEPT_CTRL=1061;# intermediate, during start or stop
let $ERROR_SERVICE_NOT_ACTIVE=1062;# service stopped

let $sc_exe= C:\Windows\System32\sc.exe;
let $ddir= $MYSQLTEST_VARDIR/tmp/$datadir_name;
let $service_name=$service_name_prefix$MASTER_MYPORT;


error 0,1;
rmdir $ddir;

--disable_result_log
error 0,$ERROR_SERVICE_DOES_NOT_EXIST;
exec $sc_exe delete $service_name;
--enable_result_log

source include/kill_mysqld.inc;
echo # run mysql_install_db with --service parameter;
--disable_result_log
exec $MYSQL_INSTALL_DB_EXE --datadir=$ddir --port=$MASTER_MYPORT --password=$password --service=$service_name -R;
--enable_result_log

echo # Start service;
--disable_result_log
exec $sc_exe start $service_name;
--enable_result_log

enable_reconnect;
source include/wait_until_connected_again.inc;
disable_reconnect;

echo # Connect with root user password=$password;
connect (con1,localhost,root,$password,mysql);

# Smoke test - check that we're actually using datadir
# we've created (i.e restart_parameters worked)
replace_result $ddir DATADIR;
select @@datadir;

echo # Stop service and wait until it is down;

# stop service
--disable_result_log
exec $sc_exe stop  $service_name;
# Wait until stopped
let $sys_errno=0;
while($sys_errno != $ERROR_SERVICE_NOT_ACTIVE)
{
  --error 0,$ERROR_SERVICE_CANNOT_ACCEPT_CTRL,$ERROR_SERVICE_NOT_ACTIVE
  exec $sc_exe stop $service_name;
  if($sys_errno != $ERROR_SERVICE_NOT_ACTIVE)
  {
    --real_sleep 0.1
  }
}
--enable_result_log

echo # Delete service;
let $sys_errno=0;
--disable_result_log
exec $sc_exe delete $service_name;
--enable_result_log

# Cleanup
source include/wait_until_disconnected.inc;
rmdir $ddir;

#restart original server
connection default;
source include/start_mysqld.inc;

