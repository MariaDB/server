# Include file for rpl_show_log_events.inc 

--let $binlog_limit=
--source suite/rpl/include/rpl_show_binlog_events.inc
--let $binlog_limit= 2
--source suite/rpl/include/rpl_show_binlog_events.inc
--let $binlog_limit= 2,4
--source suite/rpl/include/rpl_show_binlog_events.inc
--let $binlog_limit=
--let $binlog_file=
--source suite/rpl/include/rpl_show_binlog_events.inc 
