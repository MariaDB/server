--source include/count_sessions.inc

if ($data_order_desc)
{
  --echo Data will be ordered in descending order
}

if (!$data_order_desc)
{
  --echo Data will be ordered in ascending order
}

# Create a table with a primary key and one secondary key as well as one
# more column
eval CREATE TABLE t1(
  pk CHAR(5),
  a CHAR(30),
  b CHAR(30),
  PRIMARY KEY(pk) COMMENT "$pk_cf",
  KEY(a)
) ENGINE=ROCKSDB COLLATE 'latin1_bin';

# Create a second identical table to validate that bulk loading different
# tables in the same session works
eval CREATE TABLE t2(
  pk CHAR(5),
  a CHAR(30),
  b CHAR(30),
  PRIMARY KEY(pk) COMMENT "$pk_cf",
  KEY(a)
) ENGINE=ROCKSDB COLLATE 'latin1_bin';

# Create a third table using partitions to validate that bulk loading works
# across a partitioned table
eval CREATE TABLE t3(
  pk CHAR(5),
  a CHAR(30),
  b CHAR(30),
  PRIMARY KEY(pk) COMMENT "$pk_cf",
  KEY(a)
) ENGINE=ROCKSDB COLLATE 'latin1_bin' PARTITION BY KEY() PARTITIONS 4;

--let $file = `SELECT CONCAT(@@datadir, "test_loadfile.txt")`

--let MTR_DATA_ORDER_DESC = $data_order_desc;

# Create a text file with data to import into the table.
# The primary key is in sorted order and the secondary keys are randomly generated
--let ROCKSDB_INFILE = $file
perl;
my $fn = $ENV{'ROCKSDB_INFILE'};
open(my $fh, '>', $fn) || die "perl open($fn): $!";
my $max = 2500000;
my $desc = $ENV{'MTR_DATA_ORDER_DESC'};
my @chars = ("A".."Z", "a".."z", "0".."9");
my @powers_of_26 = (26 * 26 * 26 * 26, 26 * 26 * 26, 26 * 26, 26, 1);
for (my $ii = 0; $ii < $max; $ii++)
{
   my $pk;
   my $tmp = $ii;
   foreach (@powers_of_26)
   {
     if ($desc == 1)
     {
       $pk .= chr(ord('z') - int($tmp / $_));
     }
     else
     {
       $pk .= chr(ord('a') + int($tmp / $_));
     }

     $tmp = $tmp % $_;
   }

   my $num = int(rand(25)) + 6;
   my $a;
   $a .= $chars[rand(@chars)] for 1..$num;

   $num = int(rand(25)) + 6;
   my $b;
   $b .= $chars[rand(@chars)] for 1..$num;
   print $fh "$pk\t$a\t$b\n";
}
close($fh);
EOF

--file_exists $file

# Make sure a snapshot held by another user doesn't block the bulk load
connect (other,localhost,root,,);
set session transaction isolation level repeatable read;
start transaction with consistent snapshot;

# Assert that there is a pending snapshot
select VALUE > 0 as 'Has opened snapshots' from information_schema.rocksdb_dbstats where stat_type='DB_NUM_SNAPSHOTS';

connection default;

# Update CF to smaller value to create multiple SST in ingestion
eval SET @@GLOBAL.ROCKSDB_UPDATE_CF_OPTIONS=
    '$pk_cf_name={write_buffer_size=8m;target_file_size_base=1m};';

set rocksdb_bulk_load=1;
set rocksdb_bulk_load_size=100000;
--disable_query_log
--echo LOAD DATA INFILE <input_file> INTO TABLE t1;
eval LOAD DATA INFILE '$file' INTO TABLE t1;
# There should be no SST being ingested
select * from t1;
--echo LOAD DATA INFILE <input_file> INTO TABLE t2;
eval LOAD DATA INFILE '$file' INTO TABLE t2;
# There should be no SST being ingested
select * from t2;
--echo LOAD DATA INFILE <input_file> INTO TABLE t3;
eval LOAD DATA INFILE '$file' INTO TABLE t3;
# There should be no SST being ingested
select * from t3;
--enable_query_log
set rocksdb_bulk_load=0;

--remove_file $file

# Make sure row count index stats are correct
--replace_column 6 # 7 # 8 # 9 #
SHOW TABLE STATUS WHERE name LIKE 't%';

ANALYZE TABLE t1, t2, t3;

--replace_column 6 # 7 # 8 # 9 #
SHOW TABLE STATUS WHERE name LIKE 't%';

# Make sure all the data is there.
select count(pk),count(a) from t1;
select count(b)           from t1;
select count(pk),count(a) from t2;
select count(b)           from t2;
select count(pk),count(a) from t3;
select count(b)           from t3;

# Create a dummy file with a bulk load extesion.  It should be removed when
# the server starts
--let $tmpext = .bulk_load.tmp
--let $MYSQLD_DATADIR= `SELECT @@datadir`
--let $datadir = $MYSQLD_DATADIR/#rocksdb
--write_file $datadir/test$tmpext
dummy data
EOF
--write_file $datadir/longfilenamethatvalidatesthatthiswillgetdeleted$tmpext
dummy data
EOF

# Show the files exists
--list_files $datadir *$tmpext

# Now restart the server and make sure it automatically removes this test file
--source include/restart_mysqld.inc

# Show the files do not exist
--list_files $datadir *$tmpext

# Cleanup
disconnect other;
DROP TABLE t1, t2, t3;

--source include/wait_until_count_sessions.inc
