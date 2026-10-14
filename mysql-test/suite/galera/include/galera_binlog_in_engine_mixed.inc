# Workload for a cluster where one node uses the binlog in InnoDB
# (--binlog-storage-engine=innodb) and the other a file binlog.
# Transactions small and larger than the binlog cache, and streaming
# replication, are run from both nodes, before and after a state transfer
# to node_2: an SST to a clean data directory, and an IST after a kill.

--connection node_1
call mtr.add_suppression("Galera support for --binlog-storage-engine is experimental");
SELECT @@log_bin, @@binlog_storage_engine;
--connection node_2
call mtr.add_suppression("Galera support for --binlog-storage-engine is experimental");
SELECT @@log_bin, @@binlog_storage_engine;

--let $node_1=node_1
--let $node_2=node_2
--source include/auto_increment_offset_save.inc

# (No while loop: mysqltest does not run a perl block, as in
# galera_wsrep_recover.inc, again in a later iteration.)
--source suite/galera/include/galera_binlog_in_engine_mixed_round.inc
--source suite/galera/include/galera_st_clean_slave.inc
--source suite/galera/include/galera_binlog_in_engine_mixed_round.inc
--source suite/galera/include/galera_st_kill_slave.inc
--source suite/galera/include/galera_binlog_in_engine_mixed_round.inc

--source include/auto_increment_offset_restore.inc
