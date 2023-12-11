if (`select count(*) = 1 from information_schema.global_variables where variable_name = 'innodb_use_scn' and variable_value like 'on%'`)
{
  skip only without innodb_use_scn=ON;
}
