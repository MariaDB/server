# Purpose:
#    Print first LINES_TO_READ from a file.
#    The environment variables SEARCH_FILE and LINES_TO_READ must be set
#    before sourcing this routine.
# Use:
#    When the test is slow ( example because of ASAN build) then it
#    may not flush the lines when 'cat' command is called and the
#    test could fail with missing lines. Hence this can be used to
#    to print first N lines.
#

perl;

use strict;

my $search_file = $ENV{SEARCH_FILE} or die "SEARCH_FILE not set";
my $lines_to_read = $ENV{LINES_TO_READ} or die "LINES_TO_READ not set";

open(FILE, '<', $search_file) or die "Can't open file $search_file: $!";

my $line_count = 0;
while ($line_count < $lines_to_read and my $line = <FILE>)
{
    print $line;
    $line_count++;
}

close(FILE);

EOF
