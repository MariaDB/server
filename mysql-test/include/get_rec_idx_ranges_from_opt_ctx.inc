set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;-- opt_context_ends)')
         from information_schema.optimizer_context);
set @records=
    (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.num_of_records')));
select *from json_table(@records,
                        '$[*]' columns(num_of_records text path '$')) as jt;
set @indexes=
    (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.indexes')));
select *from json_table(
    @indexes, '$[*][*]' columns(index_name text path '$.index_name',
                                rec_per_key json path '$.rec_per_key')) as jt;
set @list_ranges=
    (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.list_ranges')));
select *from json_table(
    @list_ranges,
    '$[*][*]' columns(index_name text path '$.index_name',
                      ranges json path '$.ranges',
                      num_rows int path '$.num_rows',
                      max_index_blocks int path '$.max_index_blocks',
                      max_row_blocks int path '$.max_row_blocks')) as jt;
