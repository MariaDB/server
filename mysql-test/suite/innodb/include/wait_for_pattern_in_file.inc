perl;
  use strict;
  use Time::HiRes qw(sleep);
  my $search_pattern= $ENV{'SEARCH_PATTERN'} or die "SEARCH_PATTERN not set";
  my $search_file= $ENV{'SEARCH_FILE'} or die "SEARCH_FILE not set";
  my $wait_counter= 600; # 60 seconds
  while (1)
  {
    my $found= 0;
    open(FILE, $search_file) or die("Unable to open '$search_file': $!\n");
    while (<FILE>) {
      if (/$search_pattern/) {
        $found= 1;
        last;
      }
    }
    close(FILE);
    last if ($found);
    $wait_counter-- or
      die "Timeout waiting for '$search_pattern' in $search_file\n";
    sleep(0.1);
  }
EOF
