if (`select count(*) = 0 from information_schema.session_variables where variable_name = 'slave_run_triggers_for_rbr'`)
{
  skip RBR triggers are not available;
}

