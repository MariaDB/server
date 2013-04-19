if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'test_sql_discovery' AND support='YES'`)
{
  --skip Test requires test_sql_discovery engine
}
