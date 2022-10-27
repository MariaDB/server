--echo list of GTID variables :
--disable_query_log
query_vertical
SELECT @@global.gtid_domain_id AS gtid_domain_id,
  @@global.gtid_binlog_pos AS gtid_binlog_pos,
  @@global.gtid_binlog_state AS gtid_binlog_state,
  @@global.gtid_current_pos AS gtid_current_pos,
  @@global.gtid_slave_pos AS gtid_slave_pos,
  @@global.wsrep_gtid_domain_id AS wsrep_gtid_domain_id,
  @@global.wsrep_gtid_mode AS wsrep_gtid_mode;
--enable_query_log
