--source load_data_init.inc
--connection master_1
set @old_spider_direct_dup_insert= @@spider_direct_dup_insert;
set session spider_direct_dup_insert= 0;
