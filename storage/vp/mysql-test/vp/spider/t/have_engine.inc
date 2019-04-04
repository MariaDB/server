if (!`SELECT count(*) FROM information_schema.engines WHERE
      (support = 'YES' OR support = 'DEFAULT') AND
      engine = '$TEST_ENGINE_TYPE'`)
{
  SELECT engine, support FROM information_schema.engines;
  --let $SKIP_REASON= "Need $TEST_ENGINE_TYPE engine"
}
