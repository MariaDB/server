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
select sleep(1);

--disable_query_log
let $wait_timeout= 300; # Override default 30 seconds with 300.
let $wait_condition = select count(*) = 0
                      as c from information_schema.rocksdb_global_info
                      where TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
--enable_query_log

--exec bash suite/rocksdb/t/sst_count_rows.sh $MYSQLTEST_VARDIR $MYSQL_SST_DUMP $no_more_deletes
eval SET GLOBAL rocksdb_compaction_sequential_deletes= $save_rocksdb_compaction_sequential_deletes;
eval SET GLOBAL rocksdb_compaction_sequential_deletes_file_size= $save_rocksdb_compaction_sequential_deletes_file_size;
eval SET GLOBAL rocksdb_compaction_sequential_deletes_window= $save_rocksdb_compaction_sequential_deletes_window;
