# Grep file for regular expression and output to STDOUT
#
# Usage:
# --let grep_file= /path/to/your/file
# --let grep_regex= your_regex_string
# --let grep_once= your_regex_string_to_match_once
# --source include/grep.inc

--perl
  die "grep_file not set!\n" unless $ENV{grep_file};
  open (my $fh, "<", "$ENV{grep_file}") or die "$ENV{grep_file}: $!\n";
  while (<$fh>)
  {
    if (/$ENV{grep_regex}/) {
      print;
    } elsif (defined $ENV{grep_once} && /$ENV{grep_once}/) {
      print;
      last;
    }
  }
  close $fh;
EOF
