if ($log_expected_matches) {
  --echo [log_grep.inc] file: $log_file pattern: $grep_pattern expected_matches: $log_expected_matches
}
if (!$log_expected_matches) {
  --echo [log_grep.inc] file: $log_file pattern: $grep_pattern
}
--let LOG_GREP_PERL_RESULT=$MYSQL_TMP_DIR/log_grep_perl_result.test
perl;

  open my $command_file, ">", "$ENV{'LOG_GREP_PERL_RESULT'}" or die "Cannot create file";

  $log_file_full_path= $ENV{'log_file_full_path'};
  $log_slow_rate_test= $ENV{'log_slow_rate_test'};
  open(FILE, "$log_file_full_path")
    or die("Cannot open file $log_file_full_path: $!\n");

  if ($log_slow_rate_test) {
    $one= 0;
    $two= 0;
    $three= 0;
    while(<FILE>) {
      $one++   if(/'connection_one'/);
      $two++   if(/'connection_two'/);
      $three++ if(/'connection_three'/);
    }
    $sum= $one + $two + $three;
    $zero= 0;
    if ($one == 0) {
      $zero++;
    }
    if ($two == 0) {
      $zero++;
    }
    if ($three == 0) {
      $zero++;
    }
    print "[log_grep.inc] sum:  $sum\n";
    print "[log_grep.inc] zero: $zero\n";
  }
  else {
    $grep_pattern=       $ENV{'grep_pattern'};
    $lines= 0;
    while(<FILE>) {
      $lines++ if (/$grep_pattern/);
    }
    $log_expected_matches= $ENV{'log_expected_matches'};
    if ($log_expected_matches) {
      if ($log_expected_matches != $lines) {
        print "[log_grep.inc] ERROR: expected matches: $log_expected_matches, actual matches: $lines\n";
        print "[log_grep.inc] log file at $log_file_full_path\n";
        close(FILE);
        open(FILE, "$log_file_full_path")
          or die("Cannot open file $log_file_full_path: $!\n");
        while (<FILE>) {
          print ;
        }
        print $command_file "--let \$log_grep_failed= 1\n";
        print $command_file "--let \$log_grep_lines= $lines\n";
      } else {
        print "[log_grep.inc] found expected match count: $log_expected_matches\n";
      }
    } else {
      print "[log_grep.inc] lines:   $lines\n";
    }
  }
  close(FILE);
  close($command_file);
EOF
--source $LOG_GREP_PERL_RESULT
--remove_file $LOG_GREP_PERL_RESULT
if ($log_grep_failed)
{
  --echo #
  --echo # ERROR: report
  --echo #
  --echo log_file: $log_file
  --echo grep_pattern: $grep_pattern
  --echo Expected_matches: $log_expected_matches
  --echo Found_matches: $log_grep_lines

  SHOW SESSION STATUS LIKE 'Slow_queries';
  SHOW GLOBAL VARIABLES LIKE 'log%';
  SHOW GLOBAL VARIABLES LIKE 'long_query_time';
  SHOW GLOBAL VARIABLES LIKE 'min_examined_row_limit';
  SHOW GLOBAL VARIABLES LIKE 'query_cache%';
  SHOW GLOBAL VARIABLES LIKE 'slow_query%';
  SHOW SESSION VARIABLES LIKE 'log%';
  SHOW SESSION VARIABLES LIKE 'long_query_time';
  SHOW SESSION VARIABLES LIKE 'min_examined_row_limit';
  SHOW SESSION VARIABLES LIKE 'query_cache%';
  SHOW SESSION VARIABLES LIKE 'slow_query%';
  --die Testcase failed! Error rapport above.
}
