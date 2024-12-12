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
#    Optionally, SEARCH_ABORT can be specified to abort the search (in error)
#    if a specific search result is found. Its value is a regular expression
#    (with an implicit start-of-string anchor '^' prepended), and the search
#    result that it will match against is either 1) "FOUND <N>", where <N> is
#    the specific number of matches found, or 2) "NOT FOUND".
#
#    Optionally, SEARCH_WAIT can be specified to wait for a specific search
#    result. Its usage mimics that of SEARCH_ABORT, in that its value is also
#    a '^'-prepended regular expression, which will be matched against the same
#    search result. The timeout can be set in SEARCH_TIMEOUT, default is 60
#    seconds.
#
#    Optionally, SEARCH_OUTPUT can be set to control the format of output.
#    Supported formats:
#     - (default)  : "FOUND n /pattern/ in FILE " or "NOT FOUND ..."
#     - "matches"  : Each match is printed, on a separate line
#     - "count"    : "FOUND n matches in FILE" or "NOT FOUND ..." (omit pattern)
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
    my $timeout=        $ENV{SEARCH_TIMEOUT} || 60;
    my @matches;
    my $res;

    my $start_time= time();
    for (;;) {
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
        @matches= ($content =~ /$search_pattern/gs);
        $res=@matches ? "FOUND " . scalar(@matches) : "NOT FOUND";

        if ($ENV{SEARCH_WAIT} and not $res =~ /^$ENV{SEARCH_WAIT}/) {
            if (time() - $start_time < $timeout) {
                # Millisceond sleep emulated with select
                select(undef, undef, undef, 0.1);
                next;
            }
            die "Timeout waiting for $ENV{SEARCH_WAIT} ".
                "for /$search_pattern/ in $ENV{SEARCH_FILE}\n";
        }
        last;
    }

    $ENV{SEARCH_FILE} =~ s{^.*?([^/\\]+)$}{$1};

    if ($ENV{SEARCH_OUTPUT} eq "matches") {
        foreach (@matches) {
            print $_ . "\n";
        }
    }
    elsif ($ENV{SEARCH_OUTPUT} eq "count")
    {
        print "$res matches in $ENV{SEARCH_FILE}\n";
    }
    elsif ($ENV{SEARCH_ABORT} and $res =~ /^$ENV{SEARCH_ABORT}/) {
        die "$res /$search_pattern/ in $ENV{SEARCH_FILE}\n";
    } else {
        print "$res /$search_pattern/ in $ENV{SEARCH_FILE}\n";
    }
EOF
