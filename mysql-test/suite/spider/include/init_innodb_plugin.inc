--error 0,1
--mkdir $MYSQL_TEST_DIR/lib/plugin
let $VERSION_COMPILE_OS_WIN=
  `SELECT IF(@@version_compile_os like 'Win%', 1, 0)`;
if ($VERSION_COMPILE_OS_WIN)
{
  --error 0,1
  --remove_file $MYSQL_TEST_DIR/lib/plugin/ha_innodb_plugin.dll
  --error 0,1
  --copy_file $MYSQL_TEST_DIR/../storage/innodb_plugin/$MTS_VS_CONFIG/ha_innodb_plugin.dll $MYSQL_TEST_DIR/lib/plugin/ha_innodb_plugin.dll
  INSTALL PLUGIN InnoDB SONAME 'ha_innodb_plugin.dll';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  --error 0,1
  --remove_file $MYSQL_TEST_DIR/lib/plugin/ha_innodb_plugin.so
  --error 0,1
  --copy_file $MYSQL_TEST_DIR/../storage/innodb_plugin/.libs/ha_innodb_plugin.so $MYSQL_TEST_DIR/lib/plugin/ha_innodb_plugin.so
  --error 0,1
  --copy_file $MYSQL_TEST_DIR/../storage/innodb_plugin/ha_innodb_plugin.so $MYSQL_TEST_DIR/lib/plugin/ha_innodb_plugin.so
  INSTALL PLUGIN InnoDB SONAME 'ha_innodb_plugin.so';
}
if (`SELECT count(*) FROM information_schema.engines WHERE
      support = 'NO' AND engine = '$TEST_ENGINE_TYPE'`)
{
  UNINSTALL PLUGIN InnoDB;
  if ($VERSION_COMPILE_OS_WIN)
  {
    --error 0,1
    --remove_file $MYSQL_TEST_DIR/lib/plugin/ha_xtradb.dll
    --error 0,1
    --copy_file $MYSQL_TEST_DIR/../storage/xtradb/$MTS_VS_CONFIG/ha_xtradb.dll $MYSQL_TEST_DIR/lib/plugin/ha_xtradb.dll
    INSTALL PLUGIN InnoDB SONAME 'ha_xtradb.dll';
  }
  if (!$VERSION_COMPILE_OS_WIN)
  {
    --error 0,1
    --remove_file $MYSQL_TEST_DIR/lib/plugin/ha_xtradb.so
    --error 0,1
    --copy_file $MYSQL_TEST_DIR/../storage/xtradb/.libs/ha_xtradb.so $MYSQL_TEST_DIR/lib/plugin/ha_xtradb.so
    --error 0,1
    --copy_file $MYSQL_TEST_DIR/../storage/xtradb/ha_xtradb.so $MYSQL_TEST_DIR/lib/plugin/ha_xtradb.so
    INSTALL PLUGIN InnoDB SONAME 'ha_xtradb.so';
  }
}
