let $INIT_TEST_ENGINE=0;
if (!`SELECT count(*) FROM information_schema.engines WHERE
      (support = 'YES' OR support = 'DEFAULT') AND
      engine = '$TEST_ENGINE_TYPE'`)
{
  if (`SELECT IF('$TEST_ENGINE_TYPE' = 'InnoDB', 1, 0)`)
  {
    --source ../include/init_innodb_plugin.inc
    let $INIT_TEST_ENGINE=1;
  }
  if (!$INIT_TEST_ENGINE)
  {
    --echo Unknown $TEST_ENGINE_TYPE engine for init
    skip Unknown $TEST_ENGINE_TYPE engine for init;
  }
}
