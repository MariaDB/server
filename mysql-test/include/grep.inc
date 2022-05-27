# Grep file for regular expression and output to STDOUT
#
# Usage:
# --let $grep_file= /path/to/your/file
# --let $grep_regex= your_regex_string
# --source include/grep.inc

--perl
  open (my $fh, "<", "$ENV{grep_file}") or die $!;
  while (<$fh>)
  {
    /$ENV{grep_regex}/ &&
      print;
  }
  close $fh;
EOF
