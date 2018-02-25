##
# $sys_var name of the variable
# $read_only - true if read-only
# $session - true if this is session, false if global-only
# $suppress_default_value -  if true, don't check the default value
# valid_values table should contain valid values
# invalid_values 
##

--eval SET @start_global_value = @@global.$sys_var
if (!$suppress_default_value)
{
  --replace_regex /[a-f0-9]{40}/#/
  SELECT @start_global_value;
  if ($session)
  {
    --eval SET @start_session_value = @@session.$sys_var
    SELECT @start_session_value;
  }
}

if (!$read_only)
{
  --echo '# Setting to valid values in global scope#'

  --let $i=1
  --let $value=query_get_value(select value from valid_values, value, $i)
  while ($value != 'No such row')
  {
    --echo "Trying to set variable @@global.$sys_var to $value"
    --eval SET @@global.$sys_var   = $value
    --eval SELECT @@global.$sys_var
    --let $v=`SELECT @@global.$sys_var`
    --source include/correctboolvalue.inc
    if (!$sticky)
    {
      if ($v != $int_value)
      {
        --echo Set @@global.$sys_var to $value but it remained set to $v
        --die Wrong variable value
      }
    }

    --echo "Setting the global scope variable back to default"
    --eval SET @@global.$sys_var = DEFAULT
    --eval SELECT @@global.$sys_var

    --inc $i
    --let $value=query_get_value(select value from valid_values, value, $i)
  }

  if ($session)
  {
    --echo '# Setting to valid values in session scope#'

    --let $i=1
    --let $value=query_get_value(select value from valid_values, value, $i)
    while ($value != 'No such row')
    {
      --echo "Trying to set variable @@session.$sys_var to $value"
      --eval SET @@session.$sys_var   = $value
      --eval SELECT @@session.$sys_var
      --let $v=`SELECT @@session.$sys_var`
      --source include/correctboolvalue.inc
      if (!$sticky)
      {
        if ($v != $int_value)
        {
          --echo Set @@session.$sys_var to $value but it remained set to $v
          --die Wrong variable value
        }
      }
      --echo "Setting the session scope variable back to default"
      --eval SET @@session.$sys_var = DEFAULT
      --eval SELECT @@session.$sys_var

      --inc $i
      --let $value=query_get_value(select value from valid_values, value, $i)
    }
  }
  if (!$session)
  {
    --echo "Trying to set variable @@session.$sys_var to 444. It should fail because it is not session."
    --Error ER_GLOBAL_VARIABLE
    --eval SET @@session.$sys_var   = 444
  }

  --echo '# Testing with invalid values in global scope #'
  ####################################################################
  #  Change the value of query_prealloc_size   to an invalid value   #
  ####################################################################
  --let $i=1
  --let $value=query_get_value(select value from invalid_values, value, $i)
  while ($value != 'No such row')
  {
    --echo "Trying to set variable @@global.$sys_var to $value"
    --Error ER_WRONG_VALUE_FOR_VAR, ER_WRONG_TYPE_FOR_VAR
    --eval SET @@global.$sys_var   = $value
    --eval SELECT @@global.$sys_var
    --inc $i
    --let $value=query_get_value(select value from invalid_values, value, $i)
  }
}

if ($read_only)
{
  --echo "Trying to set variable @@global.$sys_var to 444. It should fail because it is readonly."
  --Error ER_INCORRECT_GLOBAL_LOCAL_VAR
  --eval SET @@global.$sys_var   = 444
}

####################################
#     Restore initial value        #
####################################
if (!$read_only)
{
  --eval SET @@global.$sys_var = @start_global_value
  --eval SELECT @@global.$sys_var
  if ($session)
  {
    --eval SET @@session.$sys_var = @start_session_value
    --eval SELECT @@session.$sys_var
  }
}
