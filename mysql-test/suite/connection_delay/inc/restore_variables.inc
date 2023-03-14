--echo # Restore original values of connection delay variables
SET @@global.password_errors_before_delay = @saved_connections_threshold;
SET @@global.max_password_errors = @saved_max_password_errors;
SET @@global.net_read_timeout = @saved_read_timeout;
SET GLOBAL DEBUG_DBUG=@old_debug;