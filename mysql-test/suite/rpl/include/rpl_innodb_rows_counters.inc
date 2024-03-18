#########################################
# Author: Benjamin Renard benj@fb.com
# Date: 11/15/2013
# Purpose: Showing the difference between current innodb rows stats and the ones recorded at the beginning of the test
# Requirements: Having @[master|slave]_[system_]rows_[read|inserted|deleted|updated] counters already created
#########################################

--connection master

select variable_value into @rows_updated from information_schema.global_status where variable_name = 'innodb_rows_updated';
select @rows_updated - @master_rows_updated;
select variable_value into @rows_deleted from information_schema.global_status where variable_name = 'innodb_rows_deleted';
select @rows_deleted - @master_rows_deleted;
select variable_value into @rows_inserted from information_schema.global_status where variable_name = 'innodb_rows_inserted';
select @rows_inserted - @master_rows_inserted;

--sync_slave_with_master

select variable_value into @rows_updated from information_schema.global_status where variable_name = 'innodb_rows_updated';
select @rows_updated - @slave_rows_updated;
select variable_value into @rows_deleted from information_schema.global_status where variable_name = 'innodb_rows_deleted';
select @rows_deleted - @slave_rows_deleted;
select variable_value into @rows_inserted from information_schema.global_status where variable_name = 'innodb_rows_inserted';
select @rows_inserted - @slave_rows_inserted;

--connection master
