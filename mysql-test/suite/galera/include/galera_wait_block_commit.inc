# include/galera_wait_block_commit.inc
#
# Wait until a statement sent on another connection waits for the backup
# lock or has ended. Run this while BACKUP STAGE BLOCK_COMMIT is active.
#
# Parameters:
#   $block_commit_id     connection ID that runs the statement
#   $block_commit_com    Com_xxx status variable of the statement
#   $block_commit_count  value of $block_commit_com before the statement
#
# Sets $block_commit_waits to 1 if the statement waits for the backup
# lock, or to 0 if it has ended.
#

--disable_query_log
# The statement has started when its Com_xxx counter has increased.
--let $wait_condition= SELECT VARIABLE_VALUE > $block_commit_count FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$block_commit_com'
--source include/wait_condition.inc
--let $wait_condition= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $block_commit_id AND (STATE = 'Waiting for backup lock' OR COMMAND <> 'Query')
--source include/wait_condition.inc
--let $block_commit_waits= `SELECT COUNT(*) FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $block_commit_id AND STATE = 'Waiting for backup lock'`
--enable_query_log
