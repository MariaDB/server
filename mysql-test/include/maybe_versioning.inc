# include file for test files that can be run with and without debug
# having debug and non-debug tests.

# If $modify_create_table is true CREATE statement must be evaluated with
# $create_options that adds WITH SYSTEM VERSIONING to the statement. Otherwise
# system versioning is added implicitly via debug options. The second variant
# can easily be added to any test but works only for debug builds.

if ($modify_create_table)
{
  if ($MTR_COMBINATION_VERS)
  {
    let $create_options= `select ' WITH SYSTEM VERSIONING'`;
  }

  if ($MTR_COMBINATION_VERS_TRX)
  {
    --skip Not tested
  }
}

if (!$modify_create_table)
{
  let $have_debug=`select version() like '%debug%'`;

  if ($MTR_COMBINATION_VERS)
  {
    if (!$have_debug)
    {
      --skip Requires debug
    }
    --disable_query_log
    set debug_dbug="d,sysvers_force_trx,sysvers_hide";
    --enable_query_log
  }

  if ($MTR_COMBINATION_VERS_TRX)
  {
    if (!$have_debug)
    {
      --skip Requires debug
    }
    --disable_query_log
    set debug_dbug="d,sysvers_force,sysvers_hide";
    --enable_query_log
  }
}
