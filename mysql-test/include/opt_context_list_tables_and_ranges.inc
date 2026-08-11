##
## Print a summary of context information_schema.optimizer_context:
##   - Tables for which we have statistics
##   - Range accesses considered
##

--echo # == Optimizer Context
--echo # === Tables
--disable_query_log
set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;#opt_context_ends)')
         from information_schema.optimizer_context);

set @table_contexts= (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.tables[*]')));
--echo # Tables in the context
select *from json_table(
    @table_contexts,
    '$[*]' columns(table_name text path '$.name',
                   file_stat_records text path '$.file_stat_records',
                   index_name text path '$**.indexes[*].index_name',
                   rec_per_key json path '$**.indexes[*].rec_per_key'
    )) as jt;

--echo # === Range accesses
set @list_ranges=
    (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.multi_range_read_info_const_calls')));
select *from json_table(
    @list_ranges,
    '$[*][*]' columns(index_name text path '$.index_name',
                      ranges json path '$.ranges',
                      num_rows int path '$.num_rows',
                      max_index_blocks int path '$.max_index_blocks',
                      max_row_blocks int path '$.max_row_blocks'
    )) as jt;
--enable_query_log
--echo # == End of optimizer context
