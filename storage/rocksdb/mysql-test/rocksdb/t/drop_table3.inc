--source include/have_rocksdb.inc

call mtr.add_suppression("Column family 'cf1' not found");
call mtr.add_suppression("Column family 'rev:cf2' not found");

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

call mtr.add_suppression("Column family 'cf1' not found");
call mtr.add_suppression("Column family 'rev:cf2' not found");

# Start from clean slate
set global rocksdb_compact_cf = 'cf1';
set global rocksdb_compact_cf = 'rev:cf2';
set global rocksdb_signal_drop_index_thread = 1;
--source include/restart_mysqld.inc

CREATE TABLE t1 (
  a int not null,
  b int not null,
  c varchar(500) not null,
  primary key (a,b) comment 'cf1',
  key (b) comment 'rev:cf2'
) ENGINE=RocksDB;

# Populate tables
let $max = 50000;
let $table = t1;
--source drop_table3_repopulate_table.inc

select variable_value into @a from information_schema.global_status where variable_name='rocksdb_compact_read_bytes';
if ($truncate_table)
{
  truncate table t1;
}
if ($drop_table)
{
  drop table t1;
}

let $show_rpl_debug_info= 1; # to force post-failure printout
let $wait_timeout= 300; # Override default 30 seconds with 300.
let $wait_condition = select count(*) = 0 
                      as c from information_schema.rocksdb_global_info
                      where TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc

select case when variable_value-@a < 500000 then 'true' else 'false' end  from information_schema.global_status where variable_name='rocksdb_compact_read_bytes';

# Cleanup
DROP TABLE IF EXISTS t1;
