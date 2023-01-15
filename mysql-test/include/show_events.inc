# ==== Purpose ====
#
# Auxiliary file used in include/show_binlog_events.inc and
# include/show_relaylog_events.inc.
#
# ==== Usage ====
#
# See include/show_binlog_events.inc

--let $include_filename= show_events.inc
--source include/begin_include_file.inc

--let $_se_old_statement= $statement

--let $statement=show BINLOG events
if ($is_relay_log)
{
  if ($master_name)
  {
    let _master_name='$master_name';
  }
  if (!$master_name)
  {
    let _master_name=`select @@default_master_connection`;
    if ($_master_name)
    {
      let _master_name='$_master_name';
    }
  }
  --let $statement=show relaylog $_master_name events
}

if ($binlog_file)
{
  --let $_binlog_file= $binlog_file
  if ($binlog_file == 'LAST')
  {
    if ($is_relay_log)
    {
      --let $_binlog_file= query_get_value(SHOW SLAVE $_master_name STATUS, Relay_Log_File, 1)
    }
    if (!$is_relay_log)
    {
      --let $_binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
    }
  }
  --let $statement= $statement in '$_binlog_file'
}

# Do not modify $binlog_start - if we did, it could wrongly persist until a
# later call of show_events.inc.
if ($binlog_start)
{
  --let $_binlog_start= $binlog_start
}

if (!$binlog_start)
{
  # If $binlog_start is not set, we will set it as the fourth event's
  # position (second in relay log which has not Binlog Checkpoint nor
  # Gtid_list events).
  # The first three events (Description Event, Gtid list, and Binlog Checkpoint
  # event) are always ignored. For description event's length might be changed
  # because of adding new events, 'SHOW BINLOG EVENTS LIMIT 3' is used to get
  # the right value.
  if ($is_relay_log)
  {
    --let $_binlog_start= query_get_value($statement LIMIT 1, End_log_pos, 1)
  }
  if (!$is_relay_log)
  {
    --let $_binlog_start= query_get_value($statement LIMIT 3, End_log_pos, 3)
  }
}

--let $statement= $statement from $_binlog_start

if ($binlog_limit != '')
{
  --let $statement= $statement limit $binlog_limit
}

# Execute the statement and write to $output_file
--let $output_file= GENERATE
--source include/write_result_to_file.inc

# Filter the file through the following script.
--delimiter ||
let $script=
  # todo: use select_columns instead (requires updating all result files)
  s{([^\t]*\t)[^\t]*(\t[^\t]*\t)[^\t]*\t[^\t]*(\t[^\t]*)}{DOLLAR1#DOLLAR2#\t#DOLLAR3};
  s{/\* xid=.* \*/}{/\* XID \*/};
  s{table_id: [0-9]+}{table_id: #};
  s{file_id=[0-9]+}{file_id=#};
  s{block_len=[0-9]+}{block_len=#};
  s{Server ver:.*DOLLAR}{SERVER_VERSION, BINLOG_VERSION};
  s{GTID [0-9]+-[0-9]+-[0-9]+}{GTID #-#-#};
  s{\[([0-9]-[0-9]-[0-9]+,?)+\]}{[#-#-#]};
  s{cid=[0-9]+}{cid=#};
  s{SQL_LOAD-[a-z,0-9,-]*.[a-z]*}{SQL_LOAD-<SERVER UUID>-<MASTER server-id>-<file-id>.<extension>};
  s{rand_seed1=[0-9]*,rand_seed2=[0-9]*}{rand_seed1=<seed 1>,rand_seed2=<seed 2>};
  s{((?:master|slave|slave-relay)-bin\.[0-9]{6};pos=)[0-9]+DOLLAR}{DOLLAR1POS};
  s{SONAME ".*"}{SONAME "LIB"};
  s{DOLLARmysqltest_vardir}{MYSQLTEST_VARDIR}g;
||
--let $pre_script= my DOLLARmysqltest_vardir = DOLLARENV{'MYSQLTEST_VARDIR'};

--delimiter ;

if ($skip_checkpoint_events)
{
  let $filter_script=Binlog_checkpoint;
}


#--let $select_columns= 1 3 6
--let $input_file= $output_file
--source include/filter_file.inc

# Write to result file
--cat_file $output_file

# Remove the file
--remove_file $output_file

--let $statement= $_se_old_statement

--let $include_filename= show_events.inc
--source include/end_include_file.inc
