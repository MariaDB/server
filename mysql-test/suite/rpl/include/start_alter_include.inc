#
# ==== Purpose ====
#
# Run concurrent split alter on different storage engine and with sync to given
# master
#
# ==== Usage ====
# --let $domain_1=
# --let $domain_2=
# --let $M_port=   //Master port
# --let $S_port=   //Slave port
# --let $sync_slave=
# --let $db_name=
#

connect(master_node,127.0.0.1,root,,$db_name, $M_port);
if (!$sync_slave)
{
  --eval set gtid_domain_id= $domain_1;
}
connect(slave_node,127.0.0.1,root,,test, $S_port);
if (!$sync_slave)
{
  --eval set gtid_domain_id= $domain_2;
}

--let $engine=myisam
--source include/start_alter_basic.inc

--let $engine=innodb
--source include/start_alter_basic.inc

--let $engine=aria
--source include/start_alter_basic.inc

--disable_query_log
--disable_warnings
--connection master_node
--echo # concurrent alter Myisam
--let $alter_engine=myisam
--let $alter_algorithm=copy
--source include/start_alter_concurrent.inc

--connection master_node
--echo # concurrent alter Aria
--let $alter_engine=aria
--let $alter_algorithm=copy
--source include/start_alter_concurrent.inc

--connection master_node
--echo # concurrent alter Innodb copy
--let $alter_engine=innodb
--let $alter_algorithm=copy
--source include/start_alter_concurrent.inc

--connection master_node
--echo # concurrent alter Innodb Inplace
--let $alter_engine=innodb
--let $alter_algorithm=inplace
--source include/start_alter_concurrent.inc
--disconnect master_node
--disconnect slave_node
--enable_warnings
--enable_query_log
