perl;
  use strict;
  use Time::HiRes qw(sleep);
  my $search_count= $ENV{'SEARCH_COUNT'} or die "SEARCH_COUNT not set";
  my $search_file= $ENV{'SEARCH_FILE'} or die "SEARCH_FILE not set";
  my $wait_counter= 100; # 10 seconds
  while (1)
  {
    my $cnt= 0;
    open(FILE, $search_file) or die("Unable to open '$search_file': $!\n");
    $cnt++ while (<FILE>);
    close(FILE);
    last if ($cnt == $search_count);
    $wait_counter-- or
      die "Timeout waiting for $search_count lines in $search_file\n";
    sleep(0.1);
  }
EOF
