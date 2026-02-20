set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;-- opt_context_ends)')
         from information_schema.optimizer_context);