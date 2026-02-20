set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;#opt_context_ends)')
         from information_schema.optimizer_context);
set @ddls=
    (select REGEXP_SUBSTR(
         context,
         '(CREATE TABLE.*)([\n\r].*)*(?=(SET join_buffer_size|REPLACE INTO))')
         from information_schema.optimizer_context);
set @fn= (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.name')));
select name from json_table(@fn, '$[*]' columns(name text path '$')) as jt;
select @ddls;