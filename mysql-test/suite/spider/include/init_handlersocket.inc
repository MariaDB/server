CALL mtr.add_suppression("open_files_limit");
--error 0,1
--mkdir $MYSQL_TEST_DIR/lib/plugin
let $VERSION_COMPILE_OS_WIN=
  `SELECT IF(@@version_compile_os like 'Win%', 1, 0)`;
if ($VERSION_COMPILE_OS_WIN)
{
  --echo Skip test with handlersocket install
  skip Skip test with handlersocket install;
}
if (!$VERSION_COMPILE_OS_WIN)
{
  --error 0,1
  --remove_file $MYSQL_TEST_DIR/lib/plugin/handlersocket.so
  --error 0,1
  --copy_file $MYSQL_TEST_DIR/../plugin/handlersocket/.libs/handlersocket.so $MYSQL_TEST_DIR/lib/plugin/handlersocket.so
  --error 0,1
  --copy_file $MYSQL_TEST_DIR/../plugin/handlersocket/handlersocket.so $MYSQL_TEST_DIR/lib/plugin/handlersocket.so
  INSTALL PLUGIN handlersocket SONAME 'handlersocket.so';
}

let $PLUGIN_VERSION=
  `SELECT SUBSTRING_INDEX(plugin_version, '.', 1)
     FROM information_schema.plugins
    WHERE plugin_name = 'handlersocket'`;
