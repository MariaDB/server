# To be used with partition mgm commands like
# ALTER TABLE t1 DROP PARTITION.
--echo # Error recovery DROP PARTITION
SET @save_dbug=@@debug_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_1";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_2";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_3";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_4";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_5";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_6";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_7";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_8";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,fail_drop_partition_9";
--source suite/parts/inc/partition_fail.inc
SET SESSION debug_dbug=@save_dbug;
