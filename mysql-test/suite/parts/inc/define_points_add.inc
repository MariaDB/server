# To be used with partition mgm commands like
# ALTER TABLE t1 ADD PARTITION (LIST/RANGE PARTITIONING).

--source suite/parts/inc/define_points_common.inc

let $points= 'add_partition_1',
  'add_partition_2',
  'add_partition_3',
  'alter_partition_alloc_parts',
  'alter_partition_add',
  'add_partition_4',
  'add_partition_5',
  'add_partition_6',
  'add_partition_7',
  'add_partition_8';

let $fail_points= $points, $fail_points, 'EOF';
let $crash_points= $points, $crash_points,
  'fail_partition_1,fail_add_partition_5,',
  'fail_partition_2,fail_add_partition_5,',
  'EOF';
