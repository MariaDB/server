# get connection and exist engine test
--echo for master_1
--source connect_master_1.inc
--connection master_1
CALL mtr.add_suppression("unknown variable");
SET SESSION sql_log_bin= 0;
--let $MASTER_1_SERVER_ID=`SELECT @@global.server_id`
--let $TEST_ENGINE_TYPE= $MASTER_1_ENGINE_TYPE
--source have_partition.inc
--source have_trigger.inc
--source ../include/init_master_1.inc
--source have_engine.inc
--echo for child2
if ($USE_CHILD_GROUP2)
{
  --echo child2_1
  --source connect_child2_1.inc
  --connection child2_1
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD2_1_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD2_1_ENGINE_TYPE
  --source ../include/init_child2_1.inc
  --source have_engine.inc
  --echo child2_2
  --source connect_child2_2.inc
  --connection child2_2
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD2_2_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD2_2_ENGINE_TYPE
  --source ../include/init_child2_2.inc
  --source have_engine.inc
  --echo child2_3
  --source connect_child2_3.inc
  --connection child2_3
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD2_3_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD2_3_ENGINE_TYPE
  --source ../include/init_child2_3.inc
  --source have_engine.inc
}
--echo for child3
if ($USE_CHILD_GROUP3)
{
  --echo child3_1
  --source connect_child3_1.inc
  --connection child3_1
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD3_1_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD3_1_ENGINE_TYPE
  --source ../include/init_child3_1.inc
  --source have_engine.inc
  --echo child3_2
  --source connect_child3_2.inc
  --connection child3_2
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD3_2_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD3_2_ENGINE_TYPE
  --source ../include/init_child3_2.inc
  --source have_engine.inc
  --echo child3_3
  --source connect_child3_3.inc
  --connection child3_3
  CALL mtr.add_suppression("unknown variable *");
  SET SESSION sql_log_bin= 0;
  --let $CHILD3_3_SERVER_ID=`SELECT @@global.server_id`
  --let $TEST_ENGINE_TYPE= $CHILD3_3_ENGINE_TYPE
  --source ../include/init_child3_3.inc
  --source have_engine.inc
}
