
-- let $engine = ROCKSDB

call mtr.add_suppression("Recovery from master pos");

-- let $debug_option = crash_before_update_pos
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_after_update_pos_before_apply
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_before_writing_xid
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = half_binlogged_transaction
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_commit_before
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_commit_after_log
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_commit_after_prepare
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_reset.inc
-- let $debug_option = crash_commit_after
-- source extra/rpl_tests/rpl_gtid_crash_safe.inc

-- source include/rpl_end.inc
