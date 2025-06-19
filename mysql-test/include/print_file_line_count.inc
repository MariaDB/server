--perl
  my $fname= "$ENV{'SEARCH_FILE'}" or die "SEARCH_FILE not set";
  open (FILE, '<', "$fname") or die("Unable to open '$fname': $!\n");
# Count lines in the SEARCH_FILE.
  my $line_count = 0;
  $line_count++ while <FILE>;
  close(FILE);

  print "$line_count\n"
EOF

# replace the timestamp and the hostname with constant values
--replace_regex /\d* \d\d:\d\d:\d\d\,[^,]*\,/TIME,HOSTNAME,/ /\,[1-9]\d*\,/,1,/ /\,[1-9]\d*/,ID/ /:\d+,/:PORT,/
cat_file $MYSQLD_DATADIR/server_audit.log;
