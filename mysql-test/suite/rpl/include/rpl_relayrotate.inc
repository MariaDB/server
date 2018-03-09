#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

#######################################################
# Wrapper for rpl_relayrotate.test to allow multi     #
# Engines to reuse test code. By JBM 2006-02-15       #
#######################################################
-- source include/have_innodb.inc
# Slow test, don't run during staging part
-- source include/not_staging.inc
-- source include/master-slave.inc

let $engine_type=innodb;
-- source suite/rpl/include/rpl_relayrotate.test
--source include/rpl_end.inc
