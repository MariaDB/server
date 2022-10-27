
source suite/rocksdb_hotbackup/include/setup.inc;

--exec suite/rocksdb_hotbackup/include/load_data.sh 2>&1
--let $rpl_server_number= 2
--source include/rpl_stop_server.inc

--error 1
--exec STREAM_TYPE=xbstream FRM=1 DEBUG_SIGNAL=1 suite/rocksdb_hotbackup/include/stream_run.sh 2>&1

--exec STREAM_TYPE=xbstream suite/rocksdb_hotbackup/include/stream_run.sh 2>&1

--let $rpl_server_number= 2
--source include/rpl_start_server.inc

connection server_2;
select count(*) from db1.t1;

connection server_1;
drop database db1;
connection server_2;
drop database db1;

source suite/rocksdb_hotbackup/include/cleanup.inc;

