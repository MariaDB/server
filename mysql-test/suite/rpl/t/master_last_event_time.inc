--echo #
--echo # Test that changes are properly applied by server_2 and server_3
--echo #

--connection server_1
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc

--echo # Show that the server_2 received the insert from master
select * from t1;

--let $master_time= query_get_value(SHOW ALL SLAVES STATUS, Master_last_event_time, 1)
--let $slave_time= query_get_value(SHOW ALL SLAVES STATUS, Slave_last_event_time, 1)
--disable_query_log
--eval select "$master_time" <> "NULL" as "master <> NULL; Should be 1"
--eval select "$master_time" = "$slave_time" as "master_time == slave_time ; Should be 1"
--enable_query_log

--connection server_3
--sleep 1
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

--echo # Show that the server_3 received the insert from master
select * from t1;

--let $master_time= query_get_value(SHOW ALL SLAVES STATUS, Master_last_event_time, 1)
--let $slave_time= query_get_value(SHOW ALL SLAVES STATUS, Slave_last_event_time, 1)
--disable_query_log
--eval select "$master_time" <> "NULL" as "master <> NULL; Should be 1"
--eval select "$master_time" = "$slave_time" as "master_time == slave_time ; Should be 1"
--enable_query_log

# Reset things for next test
--source include/stop_slave.inc
--connection server_1
