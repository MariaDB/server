set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;#opt_context_ends)')
         from information_schema.optimizer_context);

set @table_contexts= (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.list_contexts[*]')));
select *from json_table(
    @table_contexts,
    '$[*]' columns(table_name text path '$.name',
                   file_stat_records text path '$.file_stat_records',
                   index_name text path '$**.indexes[*].index_name',
                   rec_per_key json path '$**.indexes[*].rec_per_key'
    )) as jt;

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
