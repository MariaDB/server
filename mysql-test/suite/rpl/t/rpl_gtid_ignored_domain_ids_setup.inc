# Set up one exact combination of master binlog state and slave GTID position
# for rpl_gtid_ignored_domain_ids.inc.  The slave must be stopped.
#
#   $binlog_state  - what to put in the master's @@GLOBAL.gtid_binlog_state
#   $slave_pos     - what to put in the slave's @@GLOBAL.gtid_slave_pos

--connection server_1
# RESET MASTER refuses to run while the dump thread of the just-stopped slave
# is still around, and that can take a while to notice by itself.
--source include/kill_binlog_dump_threads.inc
RESET MASTER;
--eval SET GLOBAL gtid_binlog_state= '$binlog_state'

--connection server_2
--eval SET GLOBAL gtid_slave_pos= '$slave_pos'
