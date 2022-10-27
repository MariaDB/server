# get connection and exist engine test
--echo for master_1
--connection master_1
--source ../include/deinit_master_1.inc
--disconnect master_1
--echo for child2
if ($USE_CHILD_GROUP2)
{
  --echo child2_1
  --connection child2_1
  --source ../include/deinit_child2_1.inc
  --disconnect child2_1
  --disconnect child2_1_2
  --echo child2_2
  --connection child2_2
  --source ../include/deinit_child2_2.inc
  --disconnect child2_2
  --disconnect child2_2_2
  --echo child2_3
  --connection child2_3
  --source ../include/deinit_child2_3.inc
  --disconnect child2_3
}
--echo for child3
if ($USE_CHILD_GROUP3)
{
  --echo child3_1
  --connection child3_1
  --source ../include/deinit_child3_1.inc
  --disconnect child3_1
  --echo child3_2
  --connection child3_2
  --source ../include/deinit_child3_2.inc
  --disconnect child3_2
  --echo child3_3
  --connection child3_3
  --source ../include/deinit_child3_3.inc
  --disconnect child3_3
}
