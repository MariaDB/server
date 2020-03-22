#
# This file will reset binary logs of master and slave
#
--connection master
--sync_slave_with_master
--source include/stop_slave.inc
reset master;
reset slave;
--connection master
reset master;

--connection slave
--source include/start_slave.inc
--connection master
