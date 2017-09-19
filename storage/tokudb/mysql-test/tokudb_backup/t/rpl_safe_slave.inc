--connection server_1
--echo ### Create temp table on master
CREATE TEMPORARY TABLE t1 (a INT);

--let $sync_slave_connection= server_2
--source include/sync_slave_sql_with_master.inc

--echo ### Setup debug_sync points and prepare for slave backup
--connection slave_2
SET SESSION debug="+d,debug_sync_abort_on_timeout";

SHOW STATUS LIKE 'Slave_open_temp_tables';

SET DEBUG_SYNC= 'tokudb_backup_wait_for_safe_slave_entered SIGNAL sse WAIT_FOR sse_continue';
SET DEBUG_SYNC= 'tokudb_backup_wait_for_temp_tables_loop_begin SIGNAL ttlb WAIT_FOR ttlb_continue';
SET DEBUG_SYNC= 'tokudb_backup_wait_for_temp_tables_loop_slave_started SIGNAL ttlss WAIT_FOR ttlss_continue EXECUTE 2';
SET DEBUG_SYNC= 'tokudb_backup_wait_for_temp_tables_loop_end SIGNAL ttle WAIT_FOR ttle_continue';

--mkdir $BACKUP_DIR_SLAVE

--echo ### Turn-on safe-slave option
SET GLOBAL tokudb_backup_safe_slave=ON;
SET GLOBAL tokudb_backup_safe_slave_timeout=30;

--echo ### Start slave backup
--disable_query_log
--send_eval SET SESSION tokudb_backup_dir='$BACKUP_DIR_SLAVE'
--enable_query_log

--connection server_2
SET SESSION debug="+d,debug_sync_abort_on_timeout";

--echo ### Wait for safe slave function to start
SET DEBUG_SYNC = "now WAIT_FOR sse";
SHOW STATUS LIKE 'Slave_open_temp_tables';
--echo ### Wait for safe slave loop start
SET DEBUG_SYNC = "now SIGNAL sse_continue WAIT_FOR ttlb";
SHOW STATUS LIKE 'Slave_open_temp_tables';
--echo ### Wait for safe thread loop point just after slave sql thread start 1
SET DEBUG_SYNC = "now SIGNAL ttlb_continue WAIT_FOR ttlss";
SHOW STATUS LIKE 'Slave_open_temp_tables';
--echo ### Wait for safe thread loop end
SET DEBUG_SYNC = "now SIGNAL ttlss_continue WAIT_FOR ttle";
SHOW STATUS LIKE 'Slave_open_temp_tables';

--echo ### Wait for safe thread loop point just after slave sql thread start 2
SET DEBUG_SYNC = "now SIGNAL ttle_continue WAIT_FOR ttlss";

--connection server_1
--echo ### Drop temp table on master
DROP TABLE t1;

--echo ### and syncronize slave
--let $sync_slave_connection= server_2
--source include/sync_slave_sql_with_master.inc

SHOW STATUS LIKE 'Slave_open_temp_tables';

--echo ### Continue backup
SET DEBUG_SYNC = "now SIGNAL ttlss_continue";

--connection slave_2
--echo ### Wait for backup finish
--reap

--let $input_file = $S_SLAVE_INFO_FILE_PATH
--source include/filter_file.inc
--echo ### Slave $SLAVE_INFO_FILE content:
--cat_file $S_SLAVE_INFO_FILE_PATH

--echo ### Delete slave backup dir
--perl
use File::Path 'rmtree';
$DDIR=$ENV{"BACKUP_DIR_SLAVE"};
rmtree([ "$DDIR" ]);
EOF

--echo ### Turn-off safe-slave option for slave
SET GLOBAL tokudb_backup_safe_slave=default;
SET GLOBAL tokudb_backup_safe_slave_timeout=default;

--connection server_1

--echo ### Turn-on safe-slave option for master
SET GLOBAL tokudb_backup_safe_slave=ON;
SET GLOBAL tokudb_backup_safe_slave_timeout=30;

--echo ### Backup master
--mkdir $BACKUP_DIR_MASTER
--disable_query_log
--eval SET SESSION tokudb_backup_dir='$BACKUP_DIR_MASTER'
--enable_query_log

--echo ### Turn-off safe-slave option for master
SET GLOBAL tokudb_backup_safe_slave=default;
SET GLOBAL tokudb_backup_safe_slave_timeout=default;

--let $input_file = $M_MASTER_INFO_FILE_PATH
--source include/filter_file.inc
--echo ### Master $MASTER_INFO_FILE content:
--cat_file $M_MASTER_INFO_FILE_PATH

--echo ### Delete master backup dir
--perl
use File::Path 'rmtree';
$DDIR=$ENV{"BACKUP_DIR_MASTER"};
rmtree([ "$DDIR" ]);
EOF

