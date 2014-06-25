if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE LOWER(variable_name) = 'innodb_have_lzo' AND variable_value = 1`)
{
  --skip Test requires InnoDB compiled with liblzo
}
