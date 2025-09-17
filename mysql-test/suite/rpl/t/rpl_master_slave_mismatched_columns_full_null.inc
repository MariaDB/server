#
# Helper file for rpl_master_slave_mismatched_columns_full.test to test various
# null column combinations
#

--echo # No NULL columns
--let $null_col=
--source rpl_master_slave_mismatched_columns_full_typeconv.inc

--echo # Column a value is NULL
--let $null_col=a
--source rpl_master_slave_mismatched_columns_full_typeconv.inc

--echo # Column b value is NULL
--let $null_col=b
--source rpl_master_slave_mismatched_columns_full_typeconv.inc

--echo # Column c value is NULL
--let $null_col=c
--source rpl_master_slave_mismatched_columns_full_typeconv.inc

--let $null_col=
