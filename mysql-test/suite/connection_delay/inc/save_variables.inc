--echo # Save original values of connection_delay variables
SET @saved_connections_threshold = @@global.password_errors_before_delay;
SET @saved_max_password_errors = @@global.max_password_errors;
SET @saved_timeout=@@global.connect_timeout;
SET @old_debug=@@global.debug_dbug;