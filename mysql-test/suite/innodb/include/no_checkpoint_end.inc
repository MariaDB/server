# Check that the latest checkpoint in the redo log files
# is not newer than the checkpoint sampled by no_checkpoint_start.inc

if (!$no_checkpoint_kill) {
--source include/kill_mysqld.inc
}

perl;
my $cp = $ENV{CHECKPOINT_LSN};
$cp =~ s/^InnoDB\t\t//;
my $log = "$ENV{MYSQLD_DATADIR}ib_logfile0";
open(LOG, "<$log") || die "Unable to open $log";
seek(LOG, 4096, 0) || die "Unable to seek $log";
die unless read(LOG, $_, 8) == 8;
my ($cp1hi,$cp1lo) = unpack("NN", $_);
seek(LOG, 8192, 0) || die "Unable to seek $log";
die unless read(LOG, $_, 8) == 8;
my ($cp2hi,$cp2lo) = unpack("NN", $_);
close(LOG);

my $cp1 = $cp1hi << 32 | $cp1lo;
my $cp2 = $cp2hi << 32 | $cp2lo;

open(OUT, ">$ENV{MYSQLTEST_VARDIR}/log/check.txt") || die;

if ($cp1 > $cp || $cp2 > $cp) {
   print OUT "--source include/start_mysqld.inc\n"
   unless $ENV{no_checkpoint_kill};
   print OUT "$ENV{CLEANUP_IF_CHECKPOINT}\n";
   print OUT "--skip Extra checkpoint 1 after $cp ($cp1,$cp2)\n";
}

close(OUT);
EOF

--source $MYSQLTEST_VARDIR/log/check.txt
--remove_file $MYSQLTEST_VARDIR/log/check.txt
