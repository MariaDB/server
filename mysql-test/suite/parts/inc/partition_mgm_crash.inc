# collection of tests which crashes the server and checks recovery.
# also using error injection to test recovery of failures.
# uses $DATADIR and $engine

--source include/have_binlog_format_statement.inc

--echo #
--echo # Bug#53676: Unexpected errors and possible table corruption on
--echo # ADD PARTITION and LOCK TABLE
--echo # Bug#53770: Server crash at handler.cc:2076 on LOAD DATA
--echo # after timed out COALESCE PARTITION
--echo # Extended crash recovery testing of fast_alter_partition_table.
call mtr.add_suppression("Attempting backtrace. You can use the following information to find out");
call mtr.add_suppression("Crashing at");
call mtr.add_suppression("Failing at");
flush tables;
let $subpartitioning= ;
if ($MTR_COMBINATION_SUBPARTITION)
{
  # `select()` is used just to put space in front of string...
  let $subpartitioning=`select(' SUBPARTITION BY HASH(a) SUBPARTITIONS 2')`;
}

let $drop_statement= ;
let $create_statement= CREATE TABLE t1 (a INT, b VARCHAR(64))
ENGINE = $engine
PARTITION BY LIST (a)$subpartitioning
(PARTITION p0 VALUES IN (0,1,2,3,4,5,6,7,8,9),
 PARTITION p10 VALUES IN (11,12,13,14,15,16,17,18,19),
 PARTITION p20 VALUES IN (25,26,27,28,29),
 PARTITION p30 VALUES IN (31,32,33,34,35,36,37,38,39));
let $insert_statement= INSERT INTO t1 VALUES
  (1, "Original from partition p0"),
  (2, "Original from partition p0"),
  (3, "Original from partition p0"),
  (4, "Original from partition p0"),
  (11, "Original from partition p1"),
  (12, "Original from partition p1"),
  (13, "Original from partition p1"),
  (14, "Original from partition p1");

--source suite/parts/inc/define_points_add.inc
let $crash_statement= ALTER TABLE t1 ADD PARTITION
(PARTITION p15 VALUES IN (20,21,22,23,24));

if ($do_crash)
{
  --source suite/parts/inc/iterate_points_crash.inc
}
let $fail_statement= $crash_statement;
if ($do_fail)
{
  --source suite/parts/inc/iterate_points_fail.inc
}

--source suite/parts/inc/define_points_drop.inc
let $crash_statement= ALTER TABLE t1 DROP PARTITION p10, p20;
if ($do_crash)
{
  --source suite/parts/inc/iterate_points_crash.inc
}
let $fail_statement= $crash_statement;
if ($do_fail)
{
  --source suite/parts/inc/iterate_points_fail.inc
}

--source suite/parts/inc/define_points_change.inc
let $crash_statement= ALTER TABLE t1 REORGANIZE PARTITION p10, p20 INTO
(PARTITION p10 VALUES IN (10,11,12,13,14,15,16),
PARTITION p15 VALUES IN (17,18,19,20,21,22,23),
PARTITION p25 VALUES IN (24,25,26,27,28,29,30));
let $fail_statement= $crash_statement;
if ($do_crash)
{
  --source suite/parts/inc/iterate_points_crash.inc
}
if ($do_fail)
{
  --source suite/parts/inc/iterate_points_fail.inc
}

if ($MTR_COMBINATION_PARTITION)
{
  let $crash_statement= ALTER TABLE t1 REBUILD PARTITION p0, p10;
  if ($do_crash)
  {
    --source suite/parts/inc/iterate_points_crash.inc
  }
  let $fail_statement= $crash_statement;
  if ($do_fail)
  {
    --source suite/parts/inc/iterate_points_fail.inc
  }
  
  --source suite/parts/inc/define_points_convout.inc
  let $crash_statement= ALTER TABLE t1 CONVERT PARTITION p10 TO TABLE tp10;
  let $drop_statement= DROP TABLE IF EXISTS tp10;
  if ($do_crash)
  {
    --source suite/parts/inc/iterate_points_crash.inc
  }
  let $fail_statement= $crash_statement;
  if ($do_fail)
  {
    --source suite/parts/inc/iterate_points_fail.inc
  }
  let $drop_statement= ;

  --source suite/parts/inc/define_points_convin.inc
  let $create_statement2= CREATE TABLE t2 (a INT, b VARCHAR(64)) ENGINE = $engine;
  let $insert_statement2= INSERT INTO t2 VALUES (40, "40 from t2"), (50, "50 from t2");
  let $crash_statement= ALTER TABLE t1 CONVERT TABLE t2 TO PARTITION p40 VALUES IN (40, 50);
  let $drop_statement= DROP TABLE IF EXISTS t2;
  let $show_statement= SHOW CREATE TABLE t2;
  let $select_statement= SELECT * FROM t2;
  if ($do_crash)
  {
    --source suite/parts/inc/iterate_points_crash.inc
  }
  let $fail_statement= $crash_statement;
  if ($do_fail)
  {
    --source suite/parts/inc/iterate_points_fail.inc
  }
  let $drop_statement= ;
  let $show_statement= ;
  let $select_statement= ;
  let $create_statement2= ;
  let $insert_statement2= ;

  --source suite/parts/inc/define_points_change.inc
  let $create_statement= CREATE TABLE t1 (a INT, b VARCHAR(64))
  ENGINE = $engine
  PARTITION BY HASH (a) PARTITIONS 5;

  let $crash_statement= ALTER TABLE t1 COALESCE PARTITION 2;
  if ($do_crash)
  {
    --source suite/parts/inc/iterate_points_crash.inc
  }
  let $fail_statement= $crash_statement;
  if ($do_fail)
  {
    --source suite/parts/inc/iterate_points_fail.inc
  }
}
