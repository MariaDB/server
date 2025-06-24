--echo #
--echo # Test that changes are properly applied by server_2 and server_3
--echo #

# MDEV-34765: Slave to master synchronization must be done via binlog
# coordinates, as GTID with statement format would cause the sync_with_master
# points to continue too early due to the gtid_slave_pos update happening
# during the Query_log_event::do_apply_event(), which is before the
# *_last_event_time fields are updated.
--connection server_1
--source include/save_master_pos.inc

--connection server_2
--source include/sync_with_master.inc

# binlog coordinate on server_2 differs from server_1's, so update the sync pos
# so server_3 can sync
--source include/save_master_pos.inc


--echo # Show that the server_2 received the insert from master
select * from t1;

--let $master_time= query_get_value(SHOW ALL SLAVES STATUS, Master_last_event_time, 1)
--let $slave_time= query_get_value(SHOW ALL SLAVES STATUS, Slave_last_event_time, 1)
--disable_query_log
--eval select "$master_time" <> "NULL" as "master <> NULL; Should be 1"

if (`SELECT NOT "$master_time" <> "NULL"`)
{
  --echo MASTER: $master_time
}

--eval select "$master_time" = "$slave_time" as "master_time == slave_time ; Should be 1"
if (`SELECT NOT "$master_time" = "$slave_time"`)
{
  --echo MASTER: $master_time SLAVE: $slave_time
}
--enable_query_log

--connection server_3
--source include/start_slave.inc
--source include/sync_with_master.inc

--echo # Show that the server_3 received the insert from master
select * from t1;

--let $master_time= query_get_value(SHOW ALL SLAVES STATUS, Master_last_event_time, 1)
--let $slave_time= query_get_value(SHOW ALL SLAVES STATUS, Slave_last_event_time, 1)
--disable_query_log

--eval select "$master_time" <> "NULL" as "master <> NULL; Should be 1"
if (`SELECT NOT "$master_time" <> "NULL"`)
{
  --echo MASTER: $master_time
}

--eval select "$master_time" = "$slave_time" as "master_time == slave_time ; Should be 1"
if (`SELECT NOT "$master_time" = "$slave_time"`)
{
  --echo MASTER: $master_time SLAVE: $slave_time
}
--enable_query_log

# Reset things for next test
--source include/stop_slave.inc
--connection server_1
