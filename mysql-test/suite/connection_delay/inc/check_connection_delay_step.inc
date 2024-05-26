# Check response delay time
--let SEARCH_FILE= $MYSQLTEST_VARDIR/log/mysqld.1.err
--let SEARCH_PATTERN=The step for connection delay is $step nanoseconds
--source include/search_pattern_in_file.inc
