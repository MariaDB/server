--connection master_1
set session spider_direct_dup_insert= @old_spider_direct_dup_insert;
--source load_data_deinit.inc
