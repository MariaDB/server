#
# Restore the various options used for SST to their original values
# so that MTR's end-of-test checks are happy.
#

--connection node_1
CALL mtr.add_suppression("Slave SQL: Error 'The MySQL server is running with the --skip-grant-tables option so it cannot execute this statement' on query");

--disable_query_log
--eval SET GLOBAL wsrep_sst_auth = '$wsrep_sst_auth_orig';
--enable_query_log

--error 0,ER_CANNOT_USER
DROP USER sst;

--connection node_2
CALL mtr.add_suppression("Slave SQL: Error 'The MySQL server is running with the --skip-grant-tables option so it cannot execute this statement' on query");
CALL mtr.add_suppression("InnoDB: Error: Table \"mysql\"\\.\"innodb_index_stats\" not found");
CALL mtr.add_suppression("Can't open and lock time zone table");
CALL mtr.add_suppression("Can't open and lock privilege tables");
CALL mtr.add_suppression("Info table is not ready to be used");
CALL mtr.add_suppression("Native table .* has the wrong structure");

--disable_query_log
--eval SET GLOBAL wsrep_sst_method = '$wsrep_sst_method_orig';
--eval SET GLOBAL wsrep_sst_receive_address = '$wsrep_sst_receive_address_orig';
--enable_query_log
