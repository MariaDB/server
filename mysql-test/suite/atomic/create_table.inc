--source include/have_debug.inc
--source include/have_sequence.inc
--source include/have_innodb.inc
--source include/not_valgrind.inc
--source include/not_embedded.inc

#
# Testing of atomic CREATE TABLE with crashes in a lot of different places
#
# We test both statement based logging and row logging as CREATE ... SELECT
# works a bit differently depending on the logging format
#
# If more than one engines are specified, the first engine will use row
# logging and second engine will use statement based logging
#
# Parameters:
# $engines         List of quoted engines separated by ,
# $crash_points    List of crash points separated by ,
# $statement_count Number of statements
# $statements      List of quoted statements to test, separated by ,
# $show_table_t1   Set to 1 to show table t1 at end of tests
# $show_table_t2   Set to 1 to show table t2 at end of test

--disable_query_log
call mtr.add_suppression("InnoDB: .* does not exist in the InnoDB internal");
let $log_bin=`select @@log_bin`;

if ($log_bin == 1)
{
  RESET MASTER;
}
# Speed up wait_until_connected_again.inc
let NO_WSREP=1;
let $save_binlog_format=`select @@binlog_format`;

let $engine_count=`select FIND_IN_SET("end", concat("$engines",",end"))-1`;
let $crash_count=`select FIND_IN_SET("end", concat("$crash_points",",end"))-1`;

let $MYSQLD_DATADIR= `SELECT @@datadir`;

--enable_query_log

create table const_table (new int, new2 int) engine=myisam;
insert into const_table values (1,1),(2,2);
flush tables;

let $old_debug=`select @@debug_dbug`;

let $e=0;
let $keep_include_silent=1;
let $grep_script=CREATE|DROP;
--disable_query_log

while ($e < $engine_count)
{
  inc $e;
  let $engine=`select ELT($e, $engines)`;
  let $default_engine=$engine;

  # Note: $extra_option is not used. This dead code here is for conformity with
  # other tests.
  let $extra_option=;
  if ($engine == "aria")
  {
    let $extra_option=transactional=1;
  }
  if ($engine == "aria_notrans")
  {
    let $default_engine="aria";
    let $extra_option=transactional=0;
  }
  --echo engine: $engine

  let $r=0;
  while ($r < $statement_count)
  {
    inc $r;
    let $statement=`select ELT($r, $statements)`;
    --echo query: $statement
    let $create_select=`select INSTR("$statement", "SELECT") > 0`;

    let $c=0;
    while ($c < $crash_count)
    {
      inc $c;
      let $crash=`select ELT($c, $crash_points)`;

      --eval set @@default_storage_engine=$default_engine
      create or replace table t2 select seq as old from seq_1_to_2;
      flush tables;

      if ($e == 1)
      {
        --source include/set_binlog_format_row.sql
      }
      if ($e == 2)
      {
        --source include/set_binlog_format_statement.sql
      }
      if ($log_bin == 1)
      {
        FLUSH BINARY LOGS;
        --let $start_binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
      }

      --echo crash point: $crash
      --exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
      --disable_reconnect
      --eval set @@debug_dbug="+d,$crash",@debug_crash_counter=1
      let $errno=0;
      --error 0,2013,1062
      --eval $statement;
      let $error=$errno;
      if ($error == 1062)
      {
         # Duplicate key test
         let $error= 0;
       }
      --enable_reconnect
      --source include/wait_until_connected_again.inc
      --disable_query_log
      --eval set @@debug_dbug="$old_debug"

      if ($error == 0)
      {
        echo "No crash!";
      }
      # Check which tables still exists
      --list_files $MYSQLD_DATADIR/test t*
      --list_files $MYSQLD_DATADIR/test *sql*

      if ($error != 0)
      {
        if ($show_table_t1 == 1)
        {
          --error 0,1146
          show create table t1;
          if ($create_select == 1)
          {
            if ($errno == 0)
            {
              select count(*) from t1;
            }
          }
        }
        if ($show_table_t2 == 1)
        {
          --error 0,1146
          show create table t2;
          if ($create_select == 1)
          {
            if ($errno == 0)
            {
              select count(*) from t2;
            }
          }
        }
      }
              
      if ($log_bin == 1)
      {
        --let $binlog_file=$start_binlog_file
        --let $binlog_output_name=master-bin.000001
        --source include/show_binlog_events.inc
        if ($error)
        {
          --let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
          --let $binlog_output_name=master-bin.000002
          if ($binlog_file != $start_binlog_file)
          {
            --source include/show_binlog_events.inc
          }
        }
      }
      # Drop the tables. The warnings will show what was dropped
      --disable_warnings
      drop table if exists t1,t2;
      --enable_warnings
    }
  }
}
--disable_warnings
drop table if exists t1,t2,const_table;
--enable_warnings
eval set @@global.binlog_format=$save_binlog_format;

--enable_query_log
