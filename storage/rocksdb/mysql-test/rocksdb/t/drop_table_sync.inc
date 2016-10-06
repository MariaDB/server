let $show_rpl_debug_info= 1; # to force post-failure printout
let $wait_timeout= 300; # Override default 30 seconds with 300.
let $wait_condition = select count(*) = 0
                      as c from information_schema.rocksdb_global_info
                      where TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
