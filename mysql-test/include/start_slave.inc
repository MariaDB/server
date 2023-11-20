# ==== Purpose ====
#
# Issues START SLAVE on the current connection. Then waits until both
# the IO and SQL threads have started, or until a timeout is reached.
#
# Please use this instead of 'START SLAVE', to reduce the risk of test
# case bugs.
#
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# --source include/start_slave.inc
#
# Parameters:
#   $slave_timeout
#     See include/wait_for_slave_param.inc
#
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= start_slave.inc
--source include/begin_include_file.inc


if (!$rpl_debug)
{
  --disable_query_log
}


#if ($start_slave_send_sighup)
#{
## Make a thread handle SIGHUP
#--let KILL_NODE_PIDFILE = `SELECT @@pid_file`
#--perl
#  my $kill_sig = $ENV{'KILL_SIGNAL_VALUE'};
#  my $pid_filename = $ENV{'KILL_NODE_PIDFILE'};
#  my $mysqld_pid = `cat $pid_filename`;
#  chomp($mysqld_pid);
#  system("kill -HUP $mysqld_pid");
#  exit(0);
#EOF
#}


START SLAVE;
source include/wait_for_slave_to_start.inc;


--let $include_filename= start_slave.inc
--source include/end_include_file.inc
