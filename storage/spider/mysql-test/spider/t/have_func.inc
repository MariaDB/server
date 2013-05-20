if (!`SELECT count(*) FROM mysql.func WHERE name = '$TEST_FUNC_NAME'`)
{
  SELECT name FROM mysql.func;
  --echo Need $TEST_FUNC_NAME function
  skip Need $TEST_FUNC_NAME function;
}
