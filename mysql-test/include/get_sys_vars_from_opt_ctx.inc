set @set_stmts=
    (select REGEXP_SUBSTR(context, '(SET .*)([\n\r].*)*(?=(CREATE DATABASE))')
         AS set_stmt from information_schema.optimizer_context);
select @set_stmts;
