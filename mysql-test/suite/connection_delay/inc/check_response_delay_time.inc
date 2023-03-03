# Check response delay time
--let SEARCH_FILE= $MYSQLTEST_VARDIR/log/mysqld.1.err
--let SEARCH_PATTERN=$USER@$HOST wait $SERVER_RESPONSE_TIME milliseconds
--source include/search_pattern_in_file.inc
