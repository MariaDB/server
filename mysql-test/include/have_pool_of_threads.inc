--source include/not_aix.inc
--require include/have_pool_of_threads.require
disable_query_log;
show variables like 'thread_handling';
enable_query_log;
