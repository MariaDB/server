# Usage:
# let $window = <window size>;
# let $deletes = <number of deletes per window>;
# let $file_size = <min size of the file>;
# --source compact_deletes_test.inc
#

let $save_rocksdb_compaction_sequential_deletes_window = `SELECT @@rocksdb_compaction_sequential_deletes_window`;
eval set global rocksdb_compaction_sequential_deletes_window=$window;
let $save_rocksdb_compaction_sequential_deletes = `SELECT @@rocksdb_compaction_sequential_deletes`;
eval set global rocksdb_compaction_sequential_deletes= $deletes;
let $save_rocksdb_compaction_sequential_deletes_file_size = `SELECT @@rocksdb_compaction_sequential_deletes_file_size`;
eval set global rocksdb_compaction_sequential_deletes_file_size=$file_size;
--disable_query_log
let $i=0;
while ($i<1000)
{
  inc $i;
  if ($secondary_only)
  {
    eval update r1 set value2=value2+1 where id1=$i;
  }
  if ($primary)
  {
    eval update r1 set id2=id2+10000 where id1=500;
  }
}
--enable_query_log
set global rocksdb_force_flush_memtable_now=1;
--sleep 1

--disable_query_log
let $wait_timeout= 300; # Override default 30 seconds with 300.
let $wait_condition = select count(*) = 0
                      as c from information_schema.rocksdb_global_info
                      where TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
--enable_query_log

let NO_MORE_DELETES=$no_more_deletes;
perl;
  $num_retries=240;
  $retry=0;
  print "wait_for_delete: $ENV{no_more_deletes}\n";
  while ($retry++ < $num_retries) {
    $total_d=$total_e=0;
    for $f (<$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/#rocksdb/*.sst>) {
      # excluding system cf
      $filename= "$ENV{MARIAROCKS_SST_DUMP} --command=scan --output_hex --file=$f";
      open(D, '-|', $filename) || die("Can't open file $filename: $!");
      while (<D>) {
        next unless /'(\d{8})/ and $1 >= 8;
        $total_d++ if /: [07]/;
        $total_e++ if /: 1/;
      }
      close D;
    }
    last if $total_e and not ($total_d and $ENV{no_more_deletes});
    sleep 1;
  }

  unless ($total_e) {
    print "No records in the database\n";
    exit;
  }

  print $total_d ? "There are deletes left\n" : "No more deletes left\n";
EOF

eval SET GLOBAL rocksdb_compaction_sequential_deletes= $save_rocksdb_compaction_sequential_deletes;
eval SET GLOBAL rocksdb_compaction_sequential_deletes_file_size= $save_rocksdb_compaction_sequential_deletes_file_size;
eval SET GLOBAL rocksdb_compaction_sequential_deletes_window= $save_rocksdb_compaction_sequential_deletes_window;
