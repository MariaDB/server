SELECT * FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME WHERE count != 0;
SELECT * FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME_READ WHERE count != 0;
SELECT * FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME_WRITE WHERE count != 0;

--disable_query_log

--let $assert_text= The sum of query counts in read and write tables must be equal to query count of the common table
SELECT SUM(COUNT) INTO @common_count FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME;
SELECT SUM(COUNT) INTO @ro_count FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME_READ;
SELECT SUM(COUNT) INTO @rw_count FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME_WRITE;
--let $assert_cond= @common_count = @ro_count + @rw_count;
--source include/assert.inc

#
# Bug 1552428: table contents should not depend on the table case name
#
--let $assert_text= QUERY_RESPONSE_TIME query counts must be equal regardless of the table name case
SELECT SUM(COUNT) INTO @common_count_lc FROM information_schema.query_response_time;
--let $assert_cond= @common_count = @common_count_lc
--source include/assert.inc

--let $assert_text= QUERY_RESPONSE_TIME_READ query counts must be equal regardless of the table name case
SELECT SUM(COUNT) INTO @ro_count_lc FROM information_schema.query_response_time_read;
--let $assert_cond= @ro_count = @ro_count_lc
--source include/assert.inc

--let $assert_text= QUERY_RESPONSE_TIME_WRITE query counts must be equal regardless of the table name case
SELECT SUM(COUNT) INTO @rw_count_lc FROM information_schema.query_response_time_write;
--let $assert_cond= @rw_count = @rw_count_lc
--source include/assert.inc

--enable_query_log
