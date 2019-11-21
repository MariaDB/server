# This file sets the default optimizer flags and optimizer variables that
# most test expects. In most cases the variables should match the community
# server defaults.
# The purpose if this file is to allow users to change these defaults
# without having to update all tests.

--disable_query_log
set @save_optimizer_switch=@@optimizer_switch;
set @save_join_cache_level=@@join_cache_level;

set optimizer_switch="index_merge=on,index_merge_union=on,index_merge_sort_union=on,index_merge_intersection=on,index_merge_sort_intersection=off,engine_condition_pushdown=off,index_condition_pushdown=on,derived_merge=on,derived_with_keys=on,firstmatch=on,loosescan=on,materialization=on,in_to_exists=on,semijoin=on,partial_match_rowid_merge=on,partial_match_table_scan=on,subquery_cache=on,mrr=off,mrr_cost_based=off,mrr_sort_keys=off,outer_join_with_cache=on,semijoin_with_cache=on,join_cache_incremental=on,join_cache_hashed=on,join_cache_bka=on,optimize_join_buffer_size=off,table_elimination=on,extended_keys=on,exists_to_in=on,orderby_uses_equalities=on,condition_pushdown_for_derived=on";

set optimizer_use_condition_selectivity=1;
set optimizer_search_depth=62;
set join_buffer_space_limit=2097152;
set join_cache_level=2;
set join_buffer_size=262144;
--enable_query_log
