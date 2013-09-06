# get connection and exist engine test
--echo for slave1_1
--source connect_slave1_1.inc
--connection slave1_1
SET SESSION sql_log_bin= 0;
--let $SLAVE1_1_SERVER_ID=`SELECT @@global.server_id`
--let $TEST_ENGINE_TYPE= $SLAVE1_1_ENGINE_TYPE
--source have_partition.inc
--source have_trigger.inc
--source ../include/init_slave1_1.inc
--source have_engine.inc
--let $SLAVE1_1_SLAVE_STATUS=`SHOW SLAVE STATUS`
if (!$SLAVE1_1_SLAVE_STATUS)
{
  eval CHANGE MASTER TO
    MASTER_HOST = '127.0.0.1',
    MASTER_USER = 'root',
    MASTER_PASSWORD = '',
    MASTER_PORT = $MASTER_1_MYPORT
  ;
}
START SLAVE;
--connection master_1
call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
--connection slave1_1
call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
if ($USE_CHILD_GROUP2)
{
  --connection child2_1
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
  --connection child2_2
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
  --connection child2_3
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
}
if ($USE_CHILD_GROUP3)
{
  --connection child3_1
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
  --connection child3_2
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
  --connection child3_3
  call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
}
