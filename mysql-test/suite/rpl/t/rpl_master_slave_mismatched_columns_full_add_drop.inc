#
# Helper file for rpl_master_slave_mismatched_columns_full.test to test various
# combinations of adding or dropping columns
#

--echo # ALTER ADD COLUMN tests
--let $add_col= 1
--let $add_col_loc= first
--source rpl_master_slave_mismatched_columns_full.inc
--let $add_col_loc= middle
--source rpl_master_slave_mismatched_columns_full.inc
--let $add_col_loc= end
--source rpl_master_slave_mismatched_columns_full.inc

--let $add_col=
--let $add_col_loc=

--echo # ALTER DROP COLUMN tests
--let $drop_col= a
--source rpl_master_slave_mismatched_columns_full.inc
--let $drop_col= b
--source rpl_master_slave_mismatched_columns_full.inc
--let $drop_col= c
--source rpl_master_slave_mismatched_columns_full.inc
--let $drop_col= pk
--source rpl_master_slave_mismatched_columns_full.inc
--let $drop_col= a
--let $drop_col_2= c
--source rpl_master_slave_mismatched_columns_full.inc

--let $drop_col=
--let $drop_col_2=