# get connection and exist engine test
--echo for slave1_1
--connection slave1_1
STOP SLAVE;
--source ../include/deinit_slave1_1.inc
--disconnect slave1_1
