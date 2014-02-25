# galera_diff.inc
# ===============
#
# Description
# -----------
# Compare the output of the given statement on all the nodes of the cluster.
#
# Parameters
# ----------
# $galera_diff_statement
#   Statement for which the output would be compared.
# 
# $galera_diff_database
#   Database against which the above statement would be executed.
#   (Default : test)
#
# $galera_diff_servers
#   Comma separated list of servers to executed the diff statement on. If not
#   set, a list of servers will be generated based on $galera_cluster_size.
#
# $galerra_debug
#   Print debug information.
#

if (!$galera_diff_statement)
{
  --die ERROR IN TEST: $galera_diff_statement must be set before sourcing include/galera_diff.inc
}

--let $_galera_diff_database = $galera_diff_database
if (!$_galera_diff_database)
{
  --let $_galera_diff_database = test 
}

--let $_galera_diff_servers= $galera_diff_servers
if (!$_galera_diff_servers)
{
  --let $_i= $galera_cluster_size
  --let $_galera_diff_servers= 
  while ($_i)
  {
    --let $_galera_diff_servers= $_i,$_galera_diff_servers
    --dec $_i
  }
}
if ($galera_debug)
{
  --echo \$galera_diff_servers= '$_galera_diff_servers'
}

if (!$galera_debug)
{
  --disable_query_log
}

# Generate file containing $galera_diff_statement. We don't pass the
# statement on the command line, because it would be subject to shell
# substitutions.
--let $write_to_file= GENERATE
--let $write_var= $galera_diff_statement
--source include/write_var_to_file.inc
--let $_galera_diff_statement_file= $write_to_file

if (!$galera_debug)
{
  --enable_query_log
}

# Compare all servers.
--let $_galera_diff_first= 1
while ($_galera_diff_servers)
{
  # Set $_galera_diff_server_i to the first number in the list
  --let $_galera_diff_server_i= `SELECT SUBSTRING_INDEX('$_galera_diff_servers', ',', 1)`
  # Remove $_galera_diff_server_i from the list
  --let $_galera_diff_servers= `SELECT SUBSTRING('$_galera_diff_servers', LENGTH('$_galera_diff_server_i') + 2)`

  # Execute statement
  --let $_galera_diff_file= $MYSQLTEST_VARDIR/tmp/_galera_diff_server-$_galera_diff_server_i.tmp
  --exec $MYSQL --defaults-group-suffix=.$_galera_diff_server_i $_galera_diff_database < $_galera_diff_statement_file > $_galera_diff_file

  # Compare
  if (!$_galera_diff_first)
  {
    if ($galera_debug)
    {
      --echo diffing $_galera_diff_file and $_galera_diff_prev_file
    }
    --diff_files $_galera_diff_file $_galera_diff_prev_file
    --remove_file $_galera_diff_prev_file
  }
  --let $_galera_diff_prev_file= $_galera_diff_file
  --let $_galera_diff_first= 0
}

# Cleanup
--remove_file $_galera_diff_prev_file
--remove_file $_galera_diff_statement_file

