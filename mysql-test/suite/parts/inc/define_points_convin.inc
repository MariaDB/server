# To be used with partition mgm commands like
# ALTER TABLE t1 CONVERT TABLE .. TO PARTITION ..

--source suite/parts/inc/define_points_common.inc

let $points= 'convert_partition_1',
  'convert_partition_2',
  'convert_partition_3',
  'convert_partition_4',
  'convert_partition_5',
  'alter_partition_alloc_parts',
  'alter_partition_rename_table',
  'convert_partition_6',
  'convert_partition_7',
  'convert_partition_8',
  'convert_partition_9';

let $fail_points= $points, $fail_points, 'EOF';
let $crash_points= $points, $crash_points,
  'fail_partition_1,fail_convert_partition_6,',
  'fail_partition_2,fail_convert_partition_6,',
  'EOF';
