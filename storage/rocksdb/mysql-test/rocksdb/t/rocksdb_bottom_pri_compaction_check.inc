perl;
  $pid_file = $ENV{pid_file};
  $print_bottom_priority = $ENV{print_bottom_priority};
  $expected_thread_counts = $ENV{expected_thread_counts};

  if(!defined($print_bottom_priority)) {
    $print_bottom_priority = 0;
  }
  if(!defined($expected_thread_counts)) {
    $expected_thread_counts = 0;
  }

  # Get PID of mysqld
  open(my $fh, '<', $pid_file) || die "Cannot open pid file $pid_file\n";
  my $pid = <$fh>;
  $pid =~ s/\s//g;
  close($fh);

  if ($pid eq "") {
    die "Couldn't retrieve PID from PID file.\n";
  }

  if ($print_bottom_priority) {
    print "Bottom thread priority:\n";
    system("cat /proc/$pid/task/*/stat | grep bottom | awk '{print \$19}'");
  }

  print "Bottom thread counts:\n";
  my $retries = 10;
  for(my $i= 0; $i <= $retries; $i++) {
    $count=`cat /proc/$pid/task/*/stat | grep bottom | wc -l`;
    chomp($count);
    if ($count != $expected_thread_counts) {
      sleep(1);
    } else {
      print "$count\n";
      last;
    }
  }
EOF
