--echo # Save original values of connection_delay variables
SET @saved_connections_threshold = @@global.failed_attempts_before_delay;
SET @saved_max_password_errors = @@global.max_password_errors;
SET @saved_wait_timeout=@@global.wait_timeout;
SET @saved_interactive_timeout=@@global.interactive_timeout;
SET @old_debug=@@global.debug_dbug;