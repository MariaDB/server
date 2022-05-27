--disable_query_log
--disable_warnings
drop procedure if exists verify_trt;
drop procedure if exists verify_trt_dummy;
drop function if exists current_row;
drop function if exists check_row;
drop function if exists current_row_ts;
drop function if exists check_row_ts;
set @@global.time_zone= default;
--enable_warnings
--enable_query_log
