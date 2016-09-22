if (! `SELECT COUNT(*) FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE LOWER(variable_name) = 'innodb_have_snappy' AND variable_value = 'ON'`)
{
  --skip Test requires InnoDB compiled with libsnappy
}
