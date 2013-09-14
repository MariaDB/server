if (!`SELECT count(*) FROM mysql.func WHERE name = '$TEST_FUNC_NAME'`)
{
  SELECT name FROM mysql.func;
  --let $SKIP_REASON= "Need $TEST_FUNC_NAME function"
}
