#
# suite.pm will make sure that all tests including this file
# will be skipped unless innodb is enabled
#
--disable_query_log
if (`select count(*) from information_schema.system_variables where variable_name='have_sanitizer' and global_value like "MSAN%"`)
{
SET STATEMENT sql_log_bin=0 FOR
call mtr.add_suppression("InnoDB: Trying to delete tablespace.*pending operations");
}
if ($VALGRIND_TEST)
{
SET STATEMENT sql_log_bin=0 FOR
call mtr.add_suppression("InnoDB: Trying to delete tablespace.*pending operations");
}
--enable_query_log
