--echo # Restore original values of connection delay variables
SET @@global.password_errors_before_delay = @saved_connections_threshold;
SET @@global.max_password_errors = @saved_max_password_errors;
SET @@global.connect_timeout = @saved_timeout;
SET @@global.debug_dbug=@old_debug;