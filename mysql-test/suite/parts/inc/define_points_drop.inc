# To be used with partition mgm commands like
# ALTER TABLE t1 DROP PARTITION

--source suite/parts/inc/define_points_common.inc

let $points= 'drop_partition_1',
  'drop_partition_2',
  'drop_partition_3',
  'drop_partition_4',
  'drop_partition_5',
  'alter_partition_rename_table',
  'drop_partition_6',
  'drop_partition_7',
  'drop_partition_8';

let $fail_points= $points, $fail_points, 'EOF';
let $crash_points= $points, $crash_points,
  'fail_partition_1,fail_drop_partition_5,',
  'fail_partition_2,fail_drop_partition_5,',
  'EOF';
