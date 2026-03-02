# Check that the latest checkpoint in the redo log files
# is not newer than the checkpoint sampled by no_checkpoint_start.inc

--source include/kill_mysqld.inc
--error 2
--exec $MYSQLD_CMD --innodb --innodb-read-only --innodb-log-recovery-start=0 --innodb-invalid-option --innodb-page-size=$INNODB_PAGE_SIZE --innodb-buffer-pool-size=21m

perl;
my $cp=0;
my $search_file= "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
open(FILE, '<', $search_file) || die("Can't open file $search_file: $!");
while(<FILE>)
{
    if (/^CURRENT_TEST:/)
    {
        $cp=0;
    }
    elsif (!$cp && /\[Warning\] innodb_read_only prevents crash recovery between (\d+) and/o)
    {
        $cp=$1;
    }
}
close(FILE);
open(OUT, ">$ENV{MYSQLTEST_VARDIR}/log/check.txt") || die;
if ($cp != $ENV{CHECKPOINT_LSN})
{
   print OUT "--source include/start_mysqld.inc\n";
   print OUT "$ENV{CLEANUP_IF_CHECKPOINT}\n";
   print OUT "--skip Unexpected checkpoint $cp != $ENV{CHECKPOINT_LSN}\n";
}
close(OUT);
EOF

--source $MYSQLTEST_VARDIR/log/check.txt
--remove_file $MYSQLTEST_VARDIR/log/check.txt
