--disable_query_log
SET STATEMENT sql_log_bin=0 FOR
call mtr.add_suppression("Plugin 'InnoDB' registration as a STORAGE ENGINE failed\\.");
SET STATEMENT sql_log_bin=0 FOR
call mtr.add_suppression("InnoDB: Plugin initialization aborted");
SET STATEMENT sql_log_bin=0 FOR
call mtr.add_suppression("InnoDB: ib_0.*\\.log does not match innodb_encrypt_log");
--enable_query_log
if (`SELECT COUNT(*)=0 FROM information_schema.global_variables where variable_name='innodb_log_archive' and variable_value='OFF'`)
{
  --skip Test requires innodb_log_archive=OFF
}
