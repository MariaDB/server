# Display deltas for InnoDB index statistics.
# Expects: $delta_suffix (e.g., 'insert', 'delete_upper_half')
# Expects: $*_prev and $*_curr set by innodb_index_stats_capture.inc

--disable_query_log
eval SELECT $lu_curr - $lu_prev AS index_lock_upgrades_$delta_suffix;
eval SELECT $pi_curr - $pi_prev AS pessimistic_insert_calls_$delta_suffix;
eval SELECT $pu_curr - $pu_prev AS pessimistic_update_calls_$delta_suffix;
eval SELECT $pe_curr - $pe_prev AS pessimistic_delete_calls_$delta_suffix;
eval SELECT $ou_curr - $ou_prev AS optim_err_underflows_$delta_suffix;
eval SELECT $oo_curr - $oo_prev AS optim_err_overflows_$delta_suffix;
eval SELECT $ix_curr - $ix_prev AS index_x_lock_calls_$delta_suffix;
eval SELECT $ps_curr - $ps_prev AS index_page_splits_$delta_suffix;
eval SELECT $ma_curr - $ma_prev AS index_page_merge_attempts_$delta_suffix;
eval SELECT $ms_curr - $ms_prev AS index_page_merge_successful_$delta_suffix;
eval SELECT $rs_curr - $rs_prev AS index_page_reorg_successful_$delta_suffix;
eval SELECT $pd_curr - $pd_prev AS index_page_discards_$delta_suffix;
--enable_query_log
