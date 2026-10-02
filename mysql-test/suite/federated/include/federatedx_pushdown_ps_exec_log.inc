# Execute the prepared statement pstmt while capturing, on the remote
# server, the statements it receives.
# Input: $exec_msg (message to echo), $log_pattern (LIKE pattern for
# the statements to show)

--source suite/federated/include/federatedx_pushdown_log_start.inc
--echo # $exec_msg
--enable_info
EXECUTE pstmt;
--disable_info
--source suite/federated/include/federatedx_pushdown_log_show.inc
