#
# Extract the Optimizer Context part from information_schema.optimizer_context
# and save it into \@opt_context user variable.
# (we cut off SET @sys_var, CREATE TABLE/VIEW statements, etc)
#
let $set_opt_context=`
select REGEXP_SUBSTR(
     context,
     '(?s)set @opt_context=\'.*?\'(?=;#opt_context_ends)')
 from information_schema.optimizer_context;
`;

--disable_query_log
if ($set_opt_context)
{
  evalp $set_opt_context;
}
if (!$set_opt_context)
{
  # REGEXP_SUBSTR returned an empty result
  set @opt_context='';
}
--enable_query_log
echo SET @opt_context=(value from information_schema.optimizer_context);
