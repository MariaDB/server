let $INIT_TEST_ENGINE=0;
if (!`SELECT count(*) FROM information_schema.engines WHERE
      (support = 'YES' OR support = 'DEFAULT') AND
      engine = '$TEST_ENGINE_TYPE'`)
{
  if (!$SKIP_REASON)
  {
    --let $SKIP_REASON= "Need $TEST_ENGINE_TYPE engine"
  }
}
