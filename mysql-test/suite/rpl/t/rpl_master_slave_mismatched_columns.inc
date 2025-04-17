#
# Test that slave can use field names to replicate row events if a slave's
# table's columns aren't organized the same as the master.
#
# Note this is based on check_type.inc, but with more extensions to support
# the slave having a different column organization.

# -----------------------------------------------------------------------------
# TODO: THIS IS STILL A WORK IN PROGRESS
# -----------------------------------------------------------------------------
#
# Parameters:
#    $source_type      Type on the master
#    $target_type      Type on the slave
#    $source_value     Value on the master (inserted into the table)
#    $target_value     Value on the slave (expected value in the table
#                      on the slave)
#    $can_convert      True if conversion shall work, false if it
#                      shall generate an error
#    $engine_type      The storage engine to be used for storing table
#                      on both master and slave
#    $add_col          Boolean.
#    $add_col_loc      One of "first", "middle", "end"
#    $drop_col         One of the existing columns, i.e. pk, a, b, or c
#
#

if (!$engine_type)
{
  # Use the default storage engine
  --let $engine_type=`SELECT @@default_storage_engine`
}

--connection server_2
let $if_is_lossy = `SELECT FIND_IN_SET('ALL_LOSSY', @@SLAVE_TYPE_CONVERSIONS)`;
let $if_is_non_lossy = `SELECT FIND_IN_SET('ALL_NON_LOSSY', @@SLAVE_TYPE_CONVERSIONS)`;

--connection server_1
eval create table t1(
  pk int not null primary key,
  a int,
  b $source_type,
  c int
) engine=$engine_type;
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc

--echo #
--echo # TODO: Handle:
--echo #    * ADD AFTER (positional)
--echo #    * DROP
--echo #
eval alter table t1 modify b $target_type;

if ($add_col)
{
  if (`SELECT strcmp("$add_col_loc","first")=0`)
  {
    --let $loc_pos= first
  }
  if (`SELECT strcmp("$add_col_loc","middle")=0`)
  {
    --let $loc_pos= after a
  }
  if (`SELECT strcmp("$add_col_loc","end")=0`)
  {
    --let $loc_pos= after c
  }

  --eval alter table t1 add column new_col int default 99 $loc_pos
}

if ($drop_col)
{
  --eval alter table t1 drop column $drop_col
}
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc

connection server_1;
--let $master_pk= 1
--let $master_a= 1000
--let $master_b= $source_value
--let $master_c= 2000
eval INSERT INTO t1 (pk, a, b, c) VALUES($master_pk, $master_a, $master_b, $master_c);
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc

--echo # Ensure server_2 and server_3 are consistent with one another..
--let $diff_tables= server_2:test.t1, server_3:test.t1
--source include/diff_tables.inc
--connection server_1
select * from t1;

--connection server_2
select * from t1;

if ($can_convert) {
  --echo # Ensure server_2 state is correct (server_3 is proven to be
  --echo #   consistent, so we don't validate its data)..

  # Index-to-table column map for value checking:
  #  4=pk, 3=a, 2=b, 1=c
  --let $cols_to_test= 4
  while ($cols_to_test)
  {
    --let $col= pk
    --let $master_val= $master_pk
    if (`SELECT $cols_to_test=3`)
    {
      --let $col= a
      --let $master_val= $master_a
    }
    if (`SELECT $cols_to_test=2`)
    {
      --let $col= b
      --let $master_val= $master_b
    }
    if (`SELECT $cols_to_test=1`)
    {
      --let $col= c
      --let $master_val= $master_c
    }

    if (`SELECT strcmp("$col","$drop_col")=0`)
    {
      --echo # Column $col was dropped, skipping its validation
    }

    if (`SELECT strcmp("$col","$drop_col")!=0`)
    {
      if (`select $col != $master_val from t1`)
      {
        --let $real_val= `SELECT $col from t1`
        --echo # Column `$col` failed validation
        --echo # Expected (on master) value: $master_val
        --echo # Real value (server_$conn_no): $real_val
        --die Server_2: Field name lookup with type conversion failed
      }
    }

    --dec $cols_to_test
  }
  --echo # ..done
}
if (!$can_convert) {
  --echo #
  --echo # TODO Needs implementing
  --echo #

  --connection server_2
  --source include/wait_for_slave_to_stop.inc
  select * from t1;

  --connection server_3
  --source include/wait_for_slave_to_stop.inc
  select * from t1;
}


--connection server_1
drop table t1;
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc
