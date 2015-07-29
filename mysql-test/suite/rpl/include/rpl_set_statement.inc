#Check if the variable is replicated correctly with "SET STATEMENT"
# Usage:
# $rpl_ssvt_var_name - the name of tested variable;
# $rpl_ssvt_var_value - the value to set; 
# $rpl_ssvt_table - the table name to insert values.

--connection master
--echo [connection master]
eval SELECT @@$rpl_ssvt_var_name;

--connection slave
--echo [connection slave]
eval SELECT @@$rpl_ssvt_var_name;

--connection master
--echo [connection master]
--disable_result_log
eval SET STATEMENT $rpl_ssvt_var_name=$rpl_ssvt_var_value FOR
       INSERT INTO $rpl_ssvt_table VALUES(@@$rpl_ssvt_var_name);
--enable_result_log
eval SELECT @@$rpl_ssvt_var_name;

--sync_slave_with_master
--echo [connection slave]
eval SELECT * FROM $rpl_ssvt_table;
eval SELECT @@$rpl_ssvt_var_name;

--connection master
--echo [connection master]
eval DELETE FROM $rpl_ssvt_table;

