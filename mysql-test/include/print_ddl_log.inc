--echo #
--echo # Reading backup ddl log file
--echo #

let MYSQLD_DATADIR= `select @@datadir`;
perl;
  $datadir= $ENV{'MYSQLD_DATADIR'};
  $id_count= 0;
  $tmp_table_count;

  open(FILE, "$datadir/ddl.log") or
    die("Unable to read log file $datadir/ddl.log: $!\n");
  while(<FILE>)
  {
    chop;
    if (/([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)[\t]([^\t]*)/)
    {
      my $date = $1;
      my $query = $2;
      my $storage = $3;
      my $partitioned = $4;
      my $db = $5;
      my $table = $6;
      my $table_ver = $7;
      my $new_storage = $8;
      my $new_partitioned = $9;
      my $new_db = $10;
      my $new_table = $11;
      my $new_table_ver = $12;
      # Fix table ids
      $table_id1= "";
      $table_id2= "";
      if (!($table_ver eq ""))
      {
        $table_id1= "id: $id{$table_ver}";
        if (!exists($id{$table_ver}))
        {
          $id_count++;
          $table_id1= "id: $id_count";
          $id{$table_ver}= $id_count;
        }
      }
      if (!($new_table_ver eq ""))
      {
        $table_id2= "id: $id{$new_table_ver}";
        if (!exists($id{$new_table_ver}))
        {
          $id_count++;
          $table_id2= "id: $id_count";
          $id{$new_table_ver}= $id_count;
        }
      }
      # Fix table names
      $table_name1= $table;
      if (substr($table_name1,0,5) eq '@0023')
      {
        if (!exists($name{$table_name1}))
        {
          $tmp_table_count++;
          $name{$table_name1}= "#sql" . $tmp_table_count;
        }
        $table_name1= $name{$table_name1};
      }
      $table_name2= $new_table;
      if (substr($table_name2,0,5) eq '@0023')
      {
        if (!exists($name{$table_name2}))
        {
          $tmp_table_count++;
          $name{$table_name2}= "#sql" . $tmp_table_count;
        }
        $table_name2= $name{$table_name2};
      }

      print "$query,$storage,$partitioned,$db,$table_name1,$table_id1,$new_storage,$new_partitioned,$new_db,$table_name2,$table_id2\n";
    }
  }
EOF
