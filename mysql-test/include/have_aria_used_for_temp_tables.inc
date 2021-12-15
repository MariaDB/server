if (!`SELECT @@aria_used_for_temp_tables`)
{
  skip Need Aria to be used for temporary tables;
}
