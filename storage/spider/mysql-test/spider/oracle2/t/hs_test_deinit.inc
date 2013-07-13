# get connection and exist engine test
--echo for master_1
--connection master_1
--source ../include/hs_deinit_master_1.inc
--echo for child2
if ($USE_CHILD_GROUP2)
{
  --echo child2_1
  --connection child2_1
  --source ../include/hs_deinit_child2_1.inc
  --echo child2_2
  --connection child2_2
  --source ../include/hs_deinit_child2_2.inc
  --echo child2_3
  --connection child2_3
  --source ../include/hs_deinit_child2_3.inc
}
