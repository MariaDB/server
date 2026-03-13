# get connection and exist engine test
--echo for slave1_1
--connection slave1_1
STOP SLAVE;
--disconnect slave1_1
