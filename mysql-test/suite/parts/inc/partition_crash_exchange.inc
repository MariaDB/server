# To be used with WL#4445: EXCHANGE PARTITION WITH TABLE.
SET @save_dbug=@@debug_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_1";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_2";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_3";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_4";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_5";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_6";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_7";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_8";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;
SET SESSION debug_dbug="+d,exchange_partition_abort_9";
--source suite/parts/inc/partition_crash_t2.inc
SET SESSION debug_dbug=@save_dbug;

