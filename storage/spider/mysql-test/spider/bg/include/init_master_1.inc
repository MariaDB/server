--source ../../include/init_spider.inc
SET spider_internal_sql_log_off= 0;
SET spider_bgs_mode= 1;
SET spider_bgs_first_read= 2;
SET spider_bgs_second_read= 2;
SET spider_split_read= 2;
SET spider_conn_recycle_mode= 2;
SET spider_multi_split_read= 1;
SET spider_reset_sql_alloc= 0;
SET spider_bulk_update_size= 40;
SET spider_direct_order_limit= 10000;
