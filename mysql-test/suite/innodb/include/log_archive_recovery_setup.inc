--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_sequence.inc
--source include/not_embedded.inc

--echo #
--echo # MDEV-42350 innodb_log_archive=ON recovery fails
--echo # at the end of the log file
--echo #

--let $archive=`SELECT @@innodb_log_archive`
SET GLOBAL innodb_max_purge_lag_wait=0, innodb_log_archive=OFF;
SELECT @@GLOBAL.innodb_encrypt_log `OFF`;

# We want just a FILE_CHECKPOINT record with no FILE_MODIFY.
SET GLOBAL innodb_file_per_table=OFF;

CREATE TABLE t1(a BIGINT PRIMARY KEY, b CHAR(255) NOT NULL)
ENGINE=InnoDB DEFAULT CHARSET latin1 STATS_PERSISTENT=0
SELECT seq a, '' b FROM seq_1_to_14000;

SET GLOBAL innodb_max_purge_lag_wait=0, innodb_log_checkpoint_now=ON;
SET @save_dbug= @@GLOBAL.debug_dbug;

--perl
open(my $f, '<', "$ENV{MARIADB_DATADIR}/ib_logfile0") or die "ib_logfile0: $!";
read($f, my $h, 16);
open(my $o, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/first_lsn.inc") or die "inc: $!";
(my $hi,$lo)=unpack('x8NN', $h);
printf $o "--let FIRST_LSN= %08x%08x\n", $hi, $lo;
print $o "--let \$capacity= ", (-s $f) - 12288, "\n";
close($f);
EOF
--source $MYSQLTEST_VARDIR/tmp/first_lsn.inc
--remove_file $MYSQLTEST_VARDIR/tmp/first_lsn.inc
SET GLOBAL innodb_log_archive=ON;
SET GLOBAL debug_dbug='+d,ib_log_checkpoint_avoid';

# WRITE records of 10 bytes (page 0) and 11 bytes (page 128 to 16511)
# will advance the LSN so that the 16-byte FILE_CHECKPOINT record of the next
# checkpoint ends at a multiple of 4096 bytes in the log file.

--let $big= query_get_value(SELECT MIN(PAGE_NUMBER) AS p FROM information_schema.INNODB_BUFFER_PAGE WHERE SPACE=0 AND PAGE_NUMBER >= 128, p, 1)
if ($big < 128)
{
  --die Wrong page number: $big<128;
}
if ($big > 16511)
{
  --die Wrong page number: $big>16511;
}
SET debug_dbug='+d,skip_page_checksum';
SET GLOBAL innodb_saved_page_number_debug=0;
--let $lsn0= query_get_value(SHOW GLOBAL STATUS LIKE 'Innodb_lsn_current', Value, 1)
SET GLOBAL innodb_fil_make_page_dirty_debug=0;
--let $lsn= query_get_value(SHOW GLOBAL STATUS LIKE 'Innodb_lsn_current', Value, 1)
evalp SELECT $lsn - $lsn0 ten;
--let $lap= `SELECT 0x$FIRST_LSN + ($lsn - 0x$FIRST_LSN) DIV $capacity * $capacity`
--let $end= `SELECT $lap + ($lsn + 16 + 36 - $lap + 4095) DIV 4096 * 4096`
--let SIZE= `SELECT $end - $lap + 12288`
if (`SELECT $SIZE < 4194304`)
{
  --die Test setup failed: less than 4 MiB of log since LSN $lap
}
--let $long= `SELECT ($end - 16 - $lsn) MOD 10`
--let $short= `SELECT ($end - 16 - $lsn - 11 * $long) DIV 10`
--disable_query_log
while ($short)
{
  SET GLOBAL innodb_fil_make_page_dirty_debug=0;
  dec $short;
}
--enable_query_log
evalp SET GLOBAL innodb_saved_page_number_debug=$big;
--disable_query_log
while ($long)
{
  SET GLOBAL innodb_fil_make_page_dirty_debug=0;
  dec $long;
}
--enable_query_log
SET debug_dbug=@save_dbug;
SET GLOBAL innodb_log_checkpoint_now=ON;
SET GLOBAL debug_dbug='+d,ib_log_checkpoint_avoid';
--let $lsn= query_get_value(SHOW GLOBAL STATUS LIKE 'Innodb_lsn_current', Value, 1)
--let NEXT_LSN= `SELECT LPAD(LOWER(HEX($lsn)), 16, '0')`
if ($lsn != $end)
{
  --die Test setup failed: the log ends at LSN $lsn instead of $end
}
