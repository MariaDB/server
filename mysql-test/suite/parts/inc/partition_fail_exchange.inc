SET @save_dbug=@@debug_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_1";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_2";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_3";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_4";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_5";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_6";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_7";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_8";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_fail_9";
--source suite/parts/inc/partition_fail_t2.inc
SET SESSION debug_dbug=@save_dbug;
