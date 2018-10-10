begin;
update t1 set c = 'MariaDB';
update t1 set c = 'InnoDB';
eval set global debug_dbug = '+d,$SEARCH_PATTERN';
commit;
--source include/shutdown_mysqld.inc
--source include/search_pattern_in_file.inc
--source include/start_mysqld.inc
