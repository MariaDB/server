# Print the "Trigger hook:" lines the server wrote to its error log since
# the previous call, without the timestamp prefix.
# Set $trg_hook_reset=1 to skip everything logged so far instead.
# Set $trg_hook_server to read mysqld.N.err instead of mysqld.1.err; the
# position of each server's log is kept in
# $MYSQLTEST_VARDIR/tmp/trigger_hook_trace.N.pos.

--let TRG_HOOK_SKIP_ALL= $trg_hook_reset
--let $trg_hook_reset= 0
--let TRG_HOOK_LOG_NO= 1
if ($trg_hook_server)
{
  --let TRG_HOOK_LOG_NO= $trg_hook_server
}
--perl
  my $server= $ENV{TRG_HOOK_LOG_NO};
  my $log= "$ENV{MYSQLTEST_VARDIR}/log/mysqld.$server.err";
  my $pos_file= "$ENV{MYSQLTEST_VARDIR}/tmp/trigger_hook_trace.$server.pos";
  my $pos= 0;
  if (open(my $p, '<', $pos_file))
  {
    $pos= <$p>;
    close($p);
  }
  open(my $f, '<', $log) or die "Cannot open $log: $!";
  seek($f, $pos, 0) unless $ENV{TRG_HOOK_SKIP_ALL};
  seek($f, 0, 2) if $ENV{TRG_HOOK_SKIP_ALL};
  while (my $line= <$f>)
  {
    print "# $1\n" if $line =~ /Trigger hook: (.*)$/;
  }
  $pos= tell($f);
  close($f);
  open(my $p, '>', $pos_file) or die "Cannot write $pos_file: $!";
  print $p $pos;
  close($p);
EOF
