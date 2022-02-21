if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'query_cache_info' and plugin_status='active'`)
{
  --skip QUERY_CACHE_INFO plugin is not active
}

set global query_cache_type=ON;
set local query_cache_type=ON;
set global query_cache_size=1355776;

create table t1 (a int not null);
insert into t1 values (1),(2),(3);
select * from t1;
select statement_schema, statement_text, result_blocks_count, result_blocks_size from information_schema.query_cache_info;

select @@time_zone into @time_zone;
select @@default_week_format into @default_week_format;
select @@character_set_client into @character_set_client;
select @@character_set_results into @character_set_results;
select @@sql_mode into @sql_mode;
select @@div_precision_increment into @div_precision_increment;
select @@lc_time_names into @lc_time_names;
select @@max_sort_length into @max_sort_length;
select @@autocommit into @autocommit;
select @@group_concat_max_len into @group_concat_max_len;
select Name into @new_time_zone from mysql.time_zone_name limit 1;
set time_zone=@new_time_zone,default_week_format=4,character_set_client='binary',character_set_results='utf32',collation_connection='utf32_bin',sql_mode='STRICT_ALL_TABLES',div_precision_increment=7,lc_time_names='ar_SD',autocommit=0, group_concat_max_len=513, max_sort_length=1011;
--disable_result_log
select * from t1;
--enable_result_log
set time_zone= @time_zone, default_week_format= @default_week_format, character_set_client= @character_set_client,character_set_results= @character_set_results, sql_mode= @sql_mode, div_precision_increment= @div_precision_increment, lc_time_names= @lc_time_names, autocommit= @autocommit, group_concat_max_len= @group_concat_max_len, max_sort_length= @max_sort_length;

--sorted_result
--replace_column 5 # 20 # 24 #
select * from information_schema.query_cache_info;
