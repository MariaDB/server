if (`select count(*) = 0 from information_schema.global_variables where variable_name = 'innodb_use_scn'`)
{
  skip innodb_use_scn is not available;
}