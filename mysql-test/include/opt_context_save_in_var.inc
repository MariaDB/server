#
# Extract the Optimizer Context part from information_schema.optimizer_context
# and save it into \@opt_context user variable.
# (we cut off SET @sys_var, CREATE TABLE/VIEW statements, etc)
#
set @opt_context=
    (select REGEXP_SUBSTR(
         context,
         '(?<=set @opt_context=\')([\n\r].*)*(?=\'\;#opt_context_ends)')
         from information_schema.optimizer_context);
