#
# ==== Purpose ====
#
# Run concurrent split alter on given storage engine
# With concurrent 10 connections
#
# ==== Usage ====
#
# --let $alter_engine= Engine Name (myisam , innodb ...)
# --let $alter_algorithm= ...
# --let $alter_online =  [][ONLINE]
# --let $domain_1=
# --let $domain_2=
# --let $M_port=   //Master port
# --let $S_port=   //Slave port
# --let $sync_slave= // 0/1 whether to sync slave with master or not
#


--connection master_node
set global debug_dbug="+d,start_alter_delay_master";

--let $i= 1
while($i < 11)
{
  if($i == 1 && $domain_1)
  {
    --eval set gtid_domain_id= $domain_1;
  }
  if($i == 6 && $domain_2)
  {
    --eval set gtid_domain_id= $domain_2;
  }
  --eval create table t$i( a int primary key, b int) engine=$alter_engine
  --eval insert into t$i values(1,1),(2,2)
  --inc $i
}


if ($sync_slave)
{
  --echo # Sync slave
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  --connection master_node
}

--let $i= 1
while($i < 21)
{
  if($i == 1 || $i == 11)
  {
    if($domain_1)
    {
      --eval set global gtid_domain_id= $domain_1;
    }
  }
  if($i == 6 || $i == 16)
  {
    if($domain_2)
    {
      --eval set global gtid_domain_id= $domain_2;
    }
  }
  connect(con$i,127.0.0.1,root,,$db_name, $M_port);
  --inc $i
}

--let $i= 1
while($i < 11)
{
  --connection con$i
  --send_eval alter $alter_online table t$i add column c int, force, algorithm=$alter_algorithm
  --inc $i
}

--connection master_node
set DEBUG_SYNC= "now signal alter_cont";

--let $i= 1
while($i < 11)
{
  --connection con$i
  --reap
  --inc $i
}
--connection master_node
set DEBUG_SYNC= 'RESET';

if ($sync_slave)
{
  --echo # Sync slave
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  --connection master_node
}


--echo # Concurrent DML
--let $i= 1
while($i < 11)
{
  --connection con$i
  --send_eval alter table t$i add column d int, force, algorithm=$alter_algorithm
  --inc $i
}

if ($alter_algorithm == "inplace")
{
  --sleep 1
  --let $i= 11
  --let $j= 1
  while($i < 21)
  {
    --connection con$i
    --send_eval insert into t$j values(5,5,5);
    --inc $i
    --inc $j
  }

  --let $i= 11
  while($i < 21)
  {
    --connection con$i
    --reap
    --inc $i
  }

  if ($sync_slave)
  {
    --echo # Sync slave
    --source include/save_master_gtid.inc
    --connection slave_node
    --source include/sync_with_master_gtid.inc
    --connection master_node
  }
}
--connection master_node
set DEBUG_SYNC= "now signal alter_cont";
--let $i= 1
while($i < 11)
{
  --connection con$i
  --reap
  --inc $i
}
--connection master_node
set DEBUG_SYNC= 'RESET';

if ($sync_slave)
{
  --echo # Sync slave
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  --connection master_node
}


--echo # Rollback tests
--let $i= 1
while($i < 11)
{
  --connection con$i
  --eval insert into t$i values(3,2,1,1)
  --send_eval alter table t$i change b b int unique, force, algorithm=$alter_algorithm
  --inc $i
}
--connection master_node
set DEBUG_SYNC= "now signal alter_cont";


--let $i= 1
while ($i < 11)
{
  --connection con$i
  --error ER_DUP_ENTRY
  --reap
  --inc $i
}
--connection master_node
set DEBUG_SYNC= 'RESET';


if ($sync_slave)
{
  --echo # Sync slave
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  --connection master_node
}

if ($sync_slave)
{
  --enable_query_log
  --echo # diff_table of master and slave , we will do only in the case when
  --echo # sync_slave is on
  --let $i= 1
  while($i < 11)
  {
    --let $diff_tables= master_node:t$i, slave_node:t$i
    source include/diff_tables.inc;
    --inc $i
  }
  --disable_query_log
}

--connection master_node
drop table t1,t2,t3,t4,t5,t6,t7,t8,t9,t10;

if ($sync_slave)
{
  --echo # Sync slave
  --source include/save_master_gtid.inc
  --connection slave_node
  --source include/sync_with_master_gtid.inc
  --connection master_node
}

--let $i= 1
while ($i < 21)
{
  --disconnect con$i
  --inc $i
}
--connection master_node
SET GLOBAL debug_dbug= "";
