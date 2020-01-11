# To be used with partition mgm commands like
# ALTER TABLE t1 COALESCE/REBUILD/REORGANIZE PARTITION.
--echo # Test change partition (REORGANIZE/REBUILD/COALESCE
--echo # or ADD HASH PARTITION).
SET @save_dbug=@@debug_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_1";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_2";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_3";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_4";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_5";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_6";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_7";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_8";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_9";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_10";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_11";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,crash_change_partition_12";
--source suite/parts/inc/partition_crash.inc
SET SESSION debug_dbug=@save_dbug;
