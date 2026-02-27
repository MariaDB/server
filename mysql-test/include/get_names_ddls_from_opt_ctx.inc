set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;-- opt_context_ends)')
         from information_schema.optimizer_context);
set @ddls= (select REGEXP_SUBSTR(
                context, '(CREATE.*)([\n\r].*)*(?=(set @opt|REPLACE INTO))')
                AS ddl from information_schema.optimizer_context);
set @db=
    (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.current_database')));
select db_used from json_table(@db,
                               '$[*]' columns(db_used text path '$')) as jt;
set @fn= (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.name')));
select name from json_table(@fn, '$[*]' columns(name text path '$')) as jt;
select @ddls;