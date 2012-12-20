if (`select count(*) < 3 from information_schema.tables 
    where table_schema = 'mysql' and table_name in ('table_stats','column_stats','index_stats')`)
{
  --skip Needs stat tables
}
