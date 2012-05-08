if (`select count(*) < 3 from information_schema.tables 
    where table_schema = 'mysql' and table_name in ('table_stat','column_stat','index_stat')`)
{
  --skip Needs stat tables
}
