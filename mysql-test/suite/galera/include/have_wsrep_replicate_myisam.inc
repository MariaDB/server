--require suite/galera/r/have_wsrep_replicate_myisam.require
disable_query_log;
SHOW VARIABLES LIKE 'wsrep_replicate_myisam';
enable_query_log;
