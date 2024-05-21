# Preparation for using no_checkpoint_end.inc

# no_checkpoint_flush: Set to trigger flushing the dirty pages from buffer pool
# and checkpoint before the "no checkpoint" block.

if ($no_checkpoint_flush) {
  --echo
  --echo # Flush all dirty pages from buffer pool
  SET @no_checkpoint_save_pct= @@GLOBAL.innodb_max_dirty_pages_pct;
  SET @no_checkpoint_save_pct_lwm= @@GLOBAL.innodb_max_dirty_pages_pct_lwm;

  SET GLOBAL innodb_max_dirty_pages_pct_lwm=0.0;
  SET GLOBAL innodb_max_dirty_pages_pct=0.0;

  let $wait_condition =
  SELECT variable_value = 0
  FROM information_schema.global_status
  WHERE variable_name = 'INNODB_BUFFER_POOL_PAGES_DIRTY';
  --source include/wait_condition.inc

  SET GLOBAL innodb_max_dirty_pages_pct= @no_checkpoint_save_pct;
  SET GLOBAL innodb_max_dirty_pages_pct_lwm= @no_checkpoint_save_pct_lwm;
  --echo
}

let MYSQLD_DATADIR= `select @@datadir`;
--replace_regex /.*Last checkpoint at[ 	]*([0-9]+).*/\1/
let CHECKPOINT_LSN=`SHOW ENGINE INNODB STATUS`;
