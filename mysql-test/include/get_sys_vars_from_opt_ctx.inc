set @set_stmts= (select REGEXP_SUBSTR(
                     context, '(SET .*)([\n\r].*)*(?=(set @opt_context=))')
                     AS set_stmt from information_schema.optimizer_context);
select @set_stmts;
