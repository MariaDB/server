if (!`SELECT count(*) FROM information_schema.plugins WHERE plugin_name = '$TEST_PLUGIN_NAME'`)
{
  SELECT plugin_name FROM information_schema.plugins;
  --let $SKIP_REASON= "Need $TEST_PLUGIN_NAME plugin"
}
