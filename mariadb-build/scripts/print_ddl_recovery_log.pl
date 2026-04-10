#!/usr/bin/perl
use warnings;
use Fcntl qw(:seek);
use Getopt::Long;

# Constants based on the source
use constant {
    BLOCK_SIZE               => 4096,
    DDL_LOG_ACTION_TYPE_POS  => 1,
    DDL_LOG_PHASE_POS        => 2,
    DDL_LOG_NEXT_ENTRY_POS   => 4,
    DDL_LOG_FLAG_POS         => 8,
    DDL_LOG_XID_POS          => 10,
    DDL_LOG_UUID_POS         => 18,
    MY_UUID_SIZE             => 16,
    DDL_LOG_ID_POS           => 34,
    DDL_LOG_END_POS          => 42,
    NAME_START_POS           => 56,
};

package main;

my @log_entrys= ("Unknown", "EXECUTE", "ENTRY", "IGNORED" );
my @log_actions= ("Unknown", "DELETE_FRM", "RENAME_FRM", "REPLACE", "EXCHANGE",
                  "RENAME_TABLE", "RENAME_VIEW", "DROP_INIT", "DROP_TABLE",
                  "DROP_VIEW", "DROP_TRIGGER", "DROP_DB", "CREATE_TABLE",
                  "CREATE_VIEW", "DELETE_TMP_FILE", "CREATE_TRIGGER",
                  "ALTER_TABLE", "STORE_QUERY");

$opt_skip_not_used= undef;
$opt_skip_ignored= undef;

sub usage
{
    print <<EOF;
Usage $0 [OPTIONS] path-to-MariaDB-ddl_recovery.log

Print the content of the MariaDB ddl_recovery.log.
One can also just provide the directory for the ddl_recover.log.

Options:
--skip-not-used\tSkip not used ddl log entries
--skip-ignored\tSkip ignored ddl log entries
EOF
    exit 0;
}

GetOptions("skip-not-used", "skip-ignored") or usage();

my $file = shift;
my $fh;

if (!defined($file))
{
    usage();
}

if (-d $file)
{
    $file= $file . "/ddl_recovery.log";
}

open $fh, '<:raw', $file or die "Cannot open $file: $!";

# Skip header block
exit 0 if (!read($fh, my $block, BLOCK_SIZE));

my $entry_num = 1;

while (read($fh, my $block, BLOCK_SIZE)) {

    my $entry_type    = unpack("C", substr($block, 0, 1));
    my $action_type   = unpack("C", substr($block, DDL_LOG_ACTION_TYPE_POS, 1));
    my $phase         = unpack("C", substr($block, DDL_LOG_PHASE_POS, 1));
    my $next_entry    = unpack("V", substr($block, DDL_LOG_NEXT_ENTRY_POS, 4));
    my $flags         = unpack("v", substr($block, DDL_LOG_FLAG_POS, 2));
    my $xid           = unpack("Q<", substr($block, DDL_LOG_XID_POS, 8));
    my $uuid_bin      = substr($block, DDL_LOG_UUID_POS, MY_UUID_SIZE);
    my $unique_id     = unpack("Q<", substr($block, DDL_LOG_ID_POS, 8));

    my $uuid = unpack("H8H4H4H4H12", $uuid_bin);
    $uuid = join('-', $uuid =~ /(.{8})(.{4})(.{4})(.{4})(.{12})/);

    my $pos = NAME_START_POS;
    my @strings;
    for (1..7) {
        my ($str, $len);
        $len = unpack("v", substr($block, $pos, 2));
        $pos += 2;
        last if ($pos + $len > BLOCK_SIZE);
        $str = substr($block, $pos, $len);
        $pos += $len+1;
        push @strings, $str;
    }

    print "\n" if ($entry_num > 1);
    print "=== DDL Log Entry $entry_num ===\n";
    $entry_num++;

    print "Entry Type     : $log_entrys[$entry_type]\n";
    next if ($opt_skip_not_used && $entry_type == 0);
    next if ($opt_skip_ignored && $entry_type >= 3);

    print "Action Type    : $log_actions[$action_type]\n";
    print "Phase          : $phase\n";
    print "Next Entry     : $next_entry\n";
    print "Flags          : $flags\n";
    print "XID            : $xid\n";
    print "UUID           : $uuid\n";
    print "Unique ID      : $unique_id\n";
    print "Handler Name   : $strings[0]\n";
    print "DB             : $strings[1]\n";
    print "Name           : $strings[2]\n";
    print "From Handler   : $strings[3]\n";
    print "From DB        : $strings[4]\n";
    print "From Name      : $strings[5]\n";
    print "Temp/Extra     : $strings[6]\n";
}

close $fh;
