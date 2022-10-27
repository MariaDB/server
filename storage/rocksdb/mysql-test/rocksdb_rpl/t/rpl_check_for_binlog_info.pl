my $pid_file = $ARGV[0];
my $log_file = $ARGV[1];

open(my $fh, '<', $pid_file) || die "Cannot open pid file $pid_file";
my $slave_pid = <$fh>;
close($fh);

$slave_pid =~ s/\s//g;
open(my $log_fh, '<', $log_file) || die "Cannot open log file $log_file";

my $pid_found = 0;
while (my $line = <$log_fh>) {
  next unless ($pid_found || $line =~ /^[\d-]* [\d:]* $slave_pid /);
  $pid_found = 1 unless ($pid_found);
  if ($line =~ /^RocksDB: Last binlog file position.*slave-bin\..*\n/) {
    print "Binlog Info Found\n";
  }
}
close($log_fh);
