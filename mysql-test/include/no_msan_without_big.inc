# Slow with MSAN, but if mtr --big-test specified, then it should complete
if (!$BIG_TEST)
{
  if (`select count(*) from information_schema.system_variables where variable_name='have_sanitizer' and global_value like "MSAN%"`)
  {
    --skip Can't be run WITH_MSAN unless using --big-test
  }
}
