#
# This file will reset binary logs of master and slave
#
--sync_slave_with_master
stop slave;
--source include/wait_for_slave_to_stop.inc
reset master;
reset slave;

--connection master
reset master;

--connection slave
start slave;
--source include/wait_for_slave_to_start.inc
