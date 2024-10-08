#
# Set all the variables required for the SST to be performed via mysqldump
#

--echo Setting SST method to mysqldump ...

call mtr.add_suppression("WSREP: wsrep_sst_method is set to 'mysqldump' yet mysqld bind_address is set to '127\\.0\\.0\\.1'");
call mtr.add_suppression("Failed to load slave replication state from table mysql\\.gtid_slave_pos");

--connection node_1
# We need a user with a password to perform SST, otherwise we hit LP #1378253
CREATE USER 'sst';
GRANT ALL PRIVILEGES ON *.* TO 'sst';

--let $wsrep_sst_auth_orig = `SELECT @@wsrep_sst_auth`
SET GLOBAL wsrep_sst_auth = 'sst:';

--connection node_2
--source include/wait_until_connected_again.inc
--let $wsrep_sst_method_orig = `SELECT @@wsrep_sst_method`
--let $wsrep_sst_receive_address_orig = `SELECT @@wsrep_sst_receive_address`

--disable_query_log
# Set wsrep_sst_receive_address to the SQL port
--eval SET GLOBAL wsrep_sst_receive_address = '127.0.0.1:$NODE_MYPORT_2';
--enable_query_log
SET GLOBAL wsrep_sst_method = 'mysqldump';
