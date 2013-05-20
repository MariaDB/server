# get connection and exist engine test
--echo for master_1
--connection master_1
--source ../include/ha_init_master_1.inc
--echo for child2
if ($USE_CHILD_GROUP2)
{
  --echo child2_1
  --connection child2_1
  --source ../include/ha_init_child2_1.inc
  --echo child2_2
  --connection child2_2
  --source ../include/ha_init_child2_2.inc
  --echo child2_3
  --connection child2_3
  --source ../include/ha_init_child2_3.inc
}
--echo for child3
if ($USE_CHILD_GROUP3)
{
  --echo child3_1
  --connection child3_1
  --source ../include/ha_init_child3_1.inc
  --echo child3_2
  --connection child3_2
  --source ../include/ha_init_child3_2.inc
  --echo child3_3
  --connection child3_3
  --source ../include/ha_init_child3_3.inc
}
