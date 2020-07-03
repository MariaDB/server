#
# Test ANALYZE TABLE that does not flush table definition cache
# Arguments:
# $percona_nonflushing_analyze_table - table to test
#

--source include/count_sessions.inc

--connect con1,localhost,root

SET DEBUG_SYNC="handler_ha_index_next_end SIGNAL idx_scan_in_progress WAIT_FOR finish_scan";

send_eval SELECT * FROM $percona_nonflushing_analyze_table;

--connection default

SET DEBUG_SYNC="now WAIT_FOR idx_scan_in_progress";

set @tmp=@@use_stat_tables;
set use_stat_tables='preferably_for_queries';
eval ANALYZE TABLE $percona_nonflushing_analyze_table;

set use_stat_tables=@tmp;
# With the bug fixed this should not block
eval SELECT * FROM $percona_nonflushing_analyze_table;

SET DEBUG_SYNC="now SIGNAL finish_scan";

--connection con1
reap;
--disconnect con1
--connection default
SET DEBUG_SYNC='reset';

--source include/wait_until_count_sessions.inc
