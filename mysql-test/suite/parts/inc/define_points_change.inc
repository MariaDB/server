# To be used with partition mgm commands like
# ALTER TABLE t1 COALESCE/REBUILD/REORGANIZE PARTITION.

--source suite/parts/inc/define_points_common.inc

let $points= 'change_partition_1',
  'change_partition_2',
  'change_partition_3',
  'alter_partition_alloc_parts',
  'alter_partition_add',
  'change_partition_add_parts_1',
  'change_partition_add_parts_2',
  'change_partition_add_parts_3',
  'change_partition_4',
  'change_partition_5',
  'alter_partition_rename_table',
  'alter_partition_rename_to_backup',
  'change_partition_rename_parts_1',
  'alter_partition_rename_added_part',
  'change_partition_rename_parts_2',
  'alter_partition_rename_drop_backup',
  'change_partition_6',
  'change_partition_7',
  'change_partition_8',
  'change_partition_9';

let $fail_points= $points, $fail_points, 'EOF';
let $crash_points= $points, $crash_points,
  'fail_partition_1,fail_change_partition_5,',
  'fail_partition_2,fail_change_partition_5,',
  'fail_partition_1,fail_change_partition_6,',
  'fail_partition_2,fail_change_partition_6,',
  'EOF';
