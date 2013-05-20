if ($INIT_TEST_ENGINE)
{
  if (`SELECT IF('$TEST_ENGINE_TYPE' = 'InnoDB', 1, 0)`)
  {
    --source ../include/deinit_innodb_plugin.inc
    let $INIT_TEST_ENGINE=0;
  }
  if ($INIT_TEST_ENGINE)
  {
    --echo Unknown $TEST_ENGINE_TYPE engine for deinit
    skip Unknown $TEST_ENGINE_TYPE engine for deinit;
  }
}
