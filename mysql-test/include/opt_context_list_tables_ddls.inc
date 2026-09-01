#
# Examine information_schema.optimizer_context and print:
#   - Tables for which we have statistics
#   - Available CREATE TABLE statements
# Those two should match.
#

--echo # == Optimizer Context Tables
--disable_query_log
set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;#opt_context_ends)')
         from information_schema.optimizer_context);
# The DDLs run up to the first REPLACE INTO, or, when there are none, up to
# the block that carries the context: "set @opt_ctx_sql_mode=..." followed
# by "set @opt_context='...". Stop at "set @opt_ctx" so that neither of
# those two statements is reported as a DDL.
set @ddls=
    (select REGEXP_SUBSTR(
         context,
         '(CREATE TABLE.*)([\n\r].*)*(?=(REPLACE INTO|set @opt_ctx))')
         from information_schema.optimizer_context);
set @fn= (select JSON_DETAILED(JSON_EXTRACT(@opt_context, '$**.name')));
select name from json_table(@fn, '$[*]' columns(name text path '$')) as jt;
--echo # === Optimizer Context DDLs
select @ddls;
--enable_query_log
