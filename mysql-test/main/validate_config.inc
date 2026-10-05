# ==== Purpose ====
#
# Run a server command, check its exit code and show or check its output
# (stdout and stderr merged).
#
# ==== Usage ====
#
# --let $vc_args= --no-defaults --validate-config
# [--let $vc_cmd= $MYSQLD_BOOTSTRAP_CMD]
# [--let $vc_rc= 2]
# [--let $vc_brief= 1]
# [--let $vc_expect= Starts the MariaDB database server]
# --source validate_config.inc
#
# Parameters:
#   $vc_args
#     Command line arguments.
#
#   $vc_cmd
#     The command to run, default $MYSQLD.
#
#   $vc_rc
#     The expected exit code, default 0.
#
#   $vc_brief
#     0 (default): print the whole output.
#     1: for long output (--help, --version) only check that $vc_expect
#     is found and $vc_absent is not.
#
#   $vc_expect, $vc_absent
#     Patterns for $vc_brief=1.
#
# $vc_rc, $vc_cmd and $vc_brief are set back to the defaults afterwards.

--let $_vc_log= $MYSQLTEST_VARDIR/tmp/validate_config.log

--error $vc_rc
--exec $vc_cmd $vc_args >$_vc_log 2>&1

if ($vc_brief)
{
  --let SEARCH_FILE= $_vc_log
  --let SEARCH_PATTERN= $vc_expect
  --source include/search_pattern_in_file.inc

  --let SEARCH_PATTERN= $vc_absent
  --let SEARCH_ABORT= FOUND
  --source include/search_pattern_in_file.inc
  --let SEARCH_ABORT=
}
if (!$vc_brief)
{
  # Remove what differs between runs and platforms: the vardir, timestamps
  # and thread id, the server name and the Index.xml path
  --replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
  --replace_regex /[0-9]+-[0-9]+-[0-9]+ +[0-9:]+ 0 // /(\S*[\\\/])?(mysqld|mariadbd)(\.exe)?: /mysqld: / /'[^']*Index\.xml'/'Index.xml'/
  --cat_file $_vc_log
}
--remove_file $_vc_log

--let $vc_rc= 0
--let $vc_cmd= $MYSQLD
--let $vc_brief= 0
