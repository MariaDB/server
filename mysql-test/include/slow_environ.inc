if (!$slow_environ_check)
{
  let $_timeout_adjustment=;
  if (`select $VALGRIND_TEST + count(*) from information_schema.system_variables where variable_name='have_sanitizer' and global_value like '%SAN%'`)
  {
    let $_timeout_adjustment=0;
  }
  let $slow_environ_check=1;
}
