# Grep file for regular expression and output to STDOUT
#
# Usage:
# --let $grep_file= /path/to/your/file
# --let $grep_regex= your_regex_string
# --let $grep_filter= filter
# --let grep_once= your_regex_string_to_match_once
# --source include/grep.inc

# Note: merge with MDEV-32188 for grep_filter by resolving in favor of this version

--perl
  die "grep_file not set!\n" unless $ENV{grep_file};
  open (my $fh, "<", "$ENV{grep_file}") or die "$ENV{grep_file}: $!\n";
  while (<$fh>)
  {
    if (defined $ENV{grep_once} && /$ENV{grep_once}/) {
      if ($ENV{grep_filter})
      {
        eval($ENV{grep_filter});
      }
      print;
      last;
    } elsif  (defined $ENV{grep_regex} && /$ENV{grep_regex}/) {
      if ($ENV{grep_filter})
      {
        eval($ENV{grep_filter});
      }
      print;
    }
  }
  close $fh;
EOF
