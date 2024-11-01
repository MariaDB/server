# Purpose:
#    Simple search with Perl for a pattern in some file.
#
#    The advantages compared to thinkable auxiliary constructs using the
#    mysqltest language and SQL are:
#    1. We do not need a running MySQL server.
#    2. SQL causes "noise" during debugging and increases the size of logs.
#       Perl code does not disturb at all.
#
#    The environment variables SEARCH_FILE and SEARCH_PATTERN must be set
#    before sourcing this routine.
#
#    Optionally, SEARCH_RANGE can be set to the max number of bytes of the file
#    to search. If negative, it will search that many bytes at the end of the
#    file. By default the search happens from the last CURRENT_TEST:
#    marker till the end of file (appropriate for searching error logs).
#
#    Optionally, SEARCH_ABORT can be set to "FOUND" or "NOT FOUND" and this
#    will abort if the search result doesn't match the requested one.
#
#    Optionally, SEARCH_OUTPUT can be set to control the format of output.
#    Supported formats:
#     - (default)  : "FOUND n /pattern/ in FILE " or "NOT FOUND ..."
#     - "matches"  : Each match is printed, on a separate line
#
#    In case of
#    - SEARCH_FILE and/or SEARCH_PATTERN is not set
#    - SEARCH_FILE cannot be opened
#    the test will abort immediate.
#
# Typical use case (check invalid server startup options):
#    let $error_log= $MYSQLTEST_VARDIR/log/my_restart.err;
#    --error 0,1
#    --remove_file $error_log
#    let SEARCH_FILE= $error_log;
#    # Stop the server
#    let $restart_file= $MYSQLTEST_VARDIR/tmp/mysqld.1.expect;
#    --write_line wait $restart_file
#    --shutdown_server
#    --source include/wait_until_disconnected.inc
#
#    --error 1
#    --exec $MYSQLD_CMD <whatever wrong setting> > $error_log 2>&1
#    # The server restart aborts
#    let SEARCH_PATTERN= \[ERROR\] Aborting;
#    --source include/search_pattern_in_file.inc
#
# Created: 2011-11-11 mleich
#

perl;
    use strict;
    die "SEARCH_FILE not set" unless $ENV{SEARCH_FILE};
    my @search_files= glob($ENV{SEARCH_FILE});
    my $search_pattern= $ENV{SEARCH_PATTERN} or die "SEARCH_PATTERN not set";
    my $search_range=   $ENV{SEARCH_RANGE};
    my $content;
    foreach my $search_file (@search_files) {
        open(FILE, '<', $search_file) || die("Can't open file $search_file: $!");
        my $file_content;
        if ($search_range > 0) {
           read(FILE, $file_content, $search_range, 0);
        } elsif ($search_range < 0) {
           my $size= -s $search_file;
           $search_range = -$size if $size > -$search_range;
           seek(FILE, $search_range, 2);
           read(FILE, $file_content, -$search_range, 0);
        } else {
          while(<FILE>) { # error log
            if (/^CURRENT_TEST:/) {
              $content='';
            } else {
              $content.=$_;
            }
          }
        }
        close(FILE);
        $content.= $file_content;
    }
    my @matches= ($content =~ /$search_pattern/gs);
    my $res=@matches ? "FOUND " . scalar(@matches) : "NOT FOUND";

    $ENV{SEARCH_FILE} =~ s{^.*?([^/\\]+)$}{$1};

    my $print_func= sub { print($_[0]); };
    my $die_func= sub { die($_[0]); };
    my $abort_func;
    my $match_func;

    # The outcome of matching is dependent on the context that we are called.
    #
    # 1) In the normal case (SEARCH_ABORT_IS_SUCCESS is NOT set), when the
    #    matching result (FOUND N or NOT FOUND) matches SEARCH_ABORT, exit in
    #    error (via die); otherwise, we assume success and print the matching
    #    result. This is used in the normal search_pattern_in_file.inc case, as
    #    well as wait_for_pattern_in_file.inc when searching that some pattern
    #    exists at all (i.e. a result of NOT FOUND will trigger die, so the
    #    script can continue waiting until the result is found, and finally
    #    print the result).
    #
    # 2) If SEARCH_ABORT_IS_SUCCESS is set, then we want to inverse the logic
    #    from case (1). That is, if the match result is SEARCH_ABORT, this
    #    is the success case, and we die for all other matching results.
    if ($ENV{SEARCH_ABORT} and $ENV{SEARCH_ABORT_IS_SUCCESS}) {
      $abort_func= \&$print_func;
      $match_func= \&$die_func;
    } else {
      $abort_func= \&$die_func;
      $match_func= \&$print_func;
    }

    if ($ENV{SEARCH_OUTPUT} eq "matches") {
        foreach (@matches) {
            print $_ . "\n";
        }
    }
    elsif ($ENV{SEARCH_ABORT} and $res =~ /^$ENV{SEARCH_ABORT}/) {
        &$abort_func("$res /$search_pattern/ in $ENV{SEARCH_FILE}\n");
    } else {
        &$match_func("$res /$search_pattern/ in $ENV{SEARCH_FILE}\n");
    }
EOF
