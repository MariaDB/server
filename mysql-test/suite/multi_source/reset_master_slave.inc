#
# The include file runs RESET ALL for every replication connection
# currently present in SHOW FULL SLAVE STATUS output on the server,
# and also runs RESET MASTER on the same server.  
#

--let $include_filename= reset_master_slave.inc
--source include/begin_include_file.inc

--disable_query_log
--disable_result_log
--disable_warnings
--let $default_master = `SELECT @@default_master_connection`
--let $con_name = query_get_value(show all slaves status, Connection_name, 1)
while ($con_name != 'No such row')
{
	eval set default_master_connection = '$con_name';
	stop slave;
	--source include/wait_for_slave_to_stop.inc
	reset slave all;
	--let $con_name = query_get_value(show all slaves status, Connection_name, 1)
}

--error 0,ER_FLUSH_MASTER_BINLOG_CLOSED
reset master;
set global gtid_slave_pos='';
eval set default_master_connection = '$default_master';
--source include/end_include_file.inc

