if (`SELECT @@GLOBAL.innodb_log_archive`)
{
  --skip Test requires innodb_log_archive=OFF
}
