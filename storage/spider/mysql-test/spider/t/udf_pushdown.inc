--echo
--echo ##### test SELECTs #####
--connection master_1
SELECT * FROM ta_l WHERE id = plusone(1);
SELECT * FROM ta_l WHERE id IN (plusone(1), plusone(2)) AND a = plusone(32);

if ($USE_CHILD_GROUP2)
{
  --connection child2_1
  SELECT argument FROM mysql.general_log WHERE argument LIKE "%select%" AND argument NOT LIKE "%argument%";
  --disable_query_log
  TRUNCATE TABLE mysql.general_log;
  --enable_query_log
}

--echo
--echo ##### test UPDATEs #####
--connection master_1
UPDATE ta_l SET a = plusone(221) WHERE id = plusone(1);
SELECT * FROM ta_l;
UPDATE ta_l SET a = plusone(332) WHERE id IN (plusone(1), plusone(2)) AND a = plusone(32);
SELECT * FROM ta_l;

if ($USE_CHILD_GROUP2)
{
  --connection child2_1
  SELECT argument FROM mysql.general_log WHERE argument LIKE "%update%" AND argument NOT LIKE "%argument%";
  --disable_query_log
  TRUNCATE TABLE mysql.general_log;
  --enable_query_log
}

--echo
--echo ##### test DELETEs #####
--connection master_1
DELETE FROM ta_l WHERE id = plusone(1);
SELECT * FROM ta_l;
DELETE FROM ta_l WHERE id IN (plusone(1), plusone(2), plusone(3)) AND a = plusone(43);
SELECT * FROM ta_l;

if ($USE_CHILD_GROUP2)
{
  --connection child2_1
  SELECT argument FROM mysql.general_log WHERE (argument LIKE "%delete%" OR argument LIKE "%update%") AND argument NOT LIKE "%argument%";
  --disable_query_log
  TRUNCATE TABLE mysql.general_log;
  --enable_query_log
}
