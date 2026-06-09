#
# Look into INFORMATION_SCHEMA.OPTIMIZER_CONTEXT and list variable definitions that it has
#
set @set_stmts=
    (select REGEXP_SUBSTR(context, '(SET .*)([\n\r].*)*(?=(CREATE DATABASE))')
         AS set_stmt from information_schema.optimizer_context);
--replace_regex /SET timestamp=[0-9.]+/SET timestamp=REPLACED/ /SET time_zone='[^']+'/SET time_zone='REPLACED'/
select @set_stmts;
