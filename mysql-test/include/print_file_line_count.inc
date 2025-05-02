--perl
  my $fname= "$ENV{'SEARCH_FILE'}" or die "SEARCH_FILE not set";

  open (FILE, '<', "$fname") or die("Unable to open '$fname': $!\n");
# Count lines in the SEARCH_FILE.
  my $line_count = 0;
  $line_count++ while <FILE>;
  close(FILE);

  print "$line_count\n"
EOF

