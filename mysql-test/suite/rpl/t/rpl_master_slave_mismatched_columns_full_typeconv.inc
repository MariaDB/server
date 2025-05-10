#
# Helper file for rpl_master_slave_mismatched_columns_full.test to vary the
# column types between master and slave
#


--connection server_2
set @saved_slave_type_conversions = @@global.slave_type_conversions;
set global slave_type_conversions='ALL_NON_LOSSY';
--connection server_3
set @saved_slave_type_conversions = @@global.slave_type_conversions;
set global slave_type_conversions='ALL_NON_LOSSY';

--connection server_1
--echo # No type conversion
--let $source_type= BLOB
--let $target_type= BLOB
--let $source_value= 'aaa'
--let $target_value= 'aaa'
--let $can_convert= 1
--source rpl_master_slave_mismatched_columns_full_add_drop.inc

--echo # Simple type conversion
--let $source_type= TINYBLOB
--let $target_type= BLOB
--let $source_value= 'aaa'
--let $target_value= 'aaa'
--let $can_convert= 1
--source rpl_master_slave_mismatched_columns_full_add_drop.inc


--connection server_2
set global slave_type_conversions= @saved_slave_type_conversions;
--connection server_3
set global slave_type_conversions= @saved_slave_type_conversions;
