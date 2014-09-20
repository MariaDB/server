if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'query_cache_queries' and plugin_status='active'`)
{
  --skip QUERY_CACHE_QUERIES plugin is not active
}
if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'query_cache_queries_tables' and plugin_status='active'`)
{
  --skip QUERY_CACHE_QUERIES_TABLES plugin is not active
}
if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'query_cache_tables' and plugin_status='active'`)
{
  --skip QUERY_CACHE_TABLES plugin is not active
}

set global query_cache_size=1355776;

create table t1 (a int not null) engine=myisam;
insert into t1 values (1),(2),(3);
select * from t1;
select statement_schema, statement_text, result_blocks_count, result_blocks_size from information_schema.query_cache_queries;
select `schema`,`table` from information_schema.query_cache_queries_tables;
select `table_schema`,`table_name` from information_schema.query_cache_tables;

