# Grep file for regular expression and output to STDOUT
#
# Usage:
# --let $grep_file= /path/to/your/file
# --let $grep_regex= your_regex_string
# --let $grep_filter= filter
# --source include/grep.inc


--perl
  open (my $fh, "<", "$ENV{grep_file}") or die $!;
  while (<$fh>)
  {
    if  (/$ENV{grep_regex}/)
    {
      if ($ENV{grep_filter})
      {
        eval($ENV{grep_filter});
      }
      print;
    }
  }
  close $fh;
EOF
