#
# This "body" file checks general properties of XA transaction replication
# as of MDEV-742.
# Parameters:
# --let rpl_xa_check= SELECT ...
#
connection master;
create table t1 (a int, b int) engine=InnoDB;
insert into t1 values(0, 0);
xa start 't';
insert into t1 values(1, 2);
xa end 't';
xa prepare 't';
xa commit 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;

xa start 't';
insert into t1 values(3, 4);
xa end 't';
xa prepare 't';
xa rollback 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;
--disable_warnings
SET pseudo_slave_mode=1;
--enable_warnings
create table t2 (a int) engine=InnoDB;
xa start 't';
insert into t1 values (5, 6);
xa end 't';
xa prepare 't';
xa start 's';
insert into t2 values (0);
xa end 's';
xa prepare 's';
--source include/save_master_gtid.inc

connection slave;
source include/sync_with_master_gtid.inc;
if ($rpl_xa_check)
{
  --eval $rpl_xa_check
  if ($rpl_xa_verbose)
  {
    --eval SELECT $rpl_xa_check_lhs
    --eval SELECT $rpl_xa_check_rhs
  }
}
sorted_result;
xa recover;

connection master;
xa commit 't';
xa commit 's';
--disable_warnings
SET pseudo_slave_mode=0;
--enable_warnings
sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;
let $diff_tables= master:t2, slave:t2;
source include/diff_tables.inc;

#
# Read-only XA remains prepared after disconnect and must rollback at XA-complete
# after recoonect. To the read-only also belongs non-transactional engine XA.
#
--connection master

--echo *** At the start of read-only section gtid list is:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc


set @query1="select 1";
set @query2="select count(*) into @s2 from t1";
--let $ro_cases=2
--let $db=test

# No disconnect
--let $p_trx=$ro_cases
while ($p_trx)
{
--connection master
  --let $xid=ro_$p_trx
  --let $query=`SELECT @query$p_trx`
  --source rpl_create_xa_prepared.inc
  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --error 0
  --disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

  --dec $p_trx
}


--let $p_trx=$ro_cases
# With diconnect
while ($p_trx)
{
--connection master
  --let $xid=ro_$p_trx
  --let $query=`SELECT @query$p_trx`
  --source rpl_create_xa_prepared.inc

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

  --dec $p_trx
}

--echo *** $ro_cases prepared xa:s must be in the list:
--connection master
sorted_result;
xa recover;

--let $p_trx=$ro_cases
while ($p_trx)
{
  --let $xid=ro_$p_trx
  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --disable_query_log
  --disable_result_log
  --error ER_XA_RBROLLBACK
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

  --dec $p_trx
}
--echo *** Zero prepared xa:s must be in the list:
xa recover;

--echo *** At the end of read-only section gtid list has 0 more compare with previous check:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc


#
# XA logging cases while some of XA resources are read-only
#
# A1. Binlog filter


--let $db=test_ign
--eval create database $db
set @@sql_log_bin = 0;
--eval create table $db.t (a int) engine=InnoDB
set @@sql_log_bin = 1;

--let $xid=rw_no_binlog
--let $query=insert into $db.t set a=1
--source rpl_create_xa_prepared.inc
--disconnect master_$xid
--source include/wait_until_disconnected.inc

--echo *** $xid must be in the list:
--connection master
xa recover;

--let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
--error 0
--disable_query_log
--disable_result_log
--eval xa $complete '$xid'
--enable_result_log
--enable_query_log

--echo *** Zero must be in the list:
--connection master
xa recover;
# restore for the following tests
--let $db=test

--echo *** At the end of --binlog-ignore-db section gtid list has 2 more:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc

#
# A2. Opposite to A1, ineffective execution in Engine may create a
#     binlog transaction
#
connection master;
create table t3 (a int) engine=innodb;

--echo *** the disconnected prepare case
--let $xid=rw_binlog_only
--let $query=delete from t3
--connect (master_$xid, 127.0.0.1,root,,$db,$MASTER_MYPORT,)
  set @@binlog_format=statement;
  # --source rpl_create_xa_prepared.inc
  --eval xa start '$xid'
  --eval $query
  --eval xa end '$xid'
  --eval xa prepare '$xid'

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

connection master;
--echo *** $xid must be in the list:
xa recover;

  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

--echo *** Zero must be in the list:
xa recover;

--echo *** the same connection complete case.
connection master;
  --let $xid=rw_binlog_only
  --let $query=delete from t3
--connect (master_$xid, 127.0.0.1,root,,$db,$MASTER_MYPORT,)
  set @@binlog_format=statement;
  # --source rpl_create_xa_prepared.inc
  --eval xa start '$xid'
  --eval $query
  --eval xa end '$xid'
  --eval xa prepare '$xid'

--echo *** $xid must be in the list:
xa recover;

--disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log
--disconnect master_$xid
--source include/wait_until_disconnected.inc

--echo *** Zero must be in the list:
--connection master
xa recover;

--echo *** At the end of ineffective in engine section gtid list has 5 more:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc

#
# A3 MyISAM "xa" logs empty XA-prepare group, followed by
#     an XA-complete event
create table tm (a int) engine=myisam;

# No disconnect
--connection master
  --let $xid=rw_myisam
  --let $query=insert into tm set a=1
  --source rpl_create_xa_prepared.inc
  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --error 0
  --disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

# With diconnect
--connection master
  --source rpl_create_xa_prepared.inc
  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

--echo *** $xid prepared must be in the list:
--connection master
xa recover;

  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

--echo *** Zero prepared xa:s must be in the list:
xa recover;

--echo *** At the end of MyISAM "xa" section gtid list has 7 more compare with previous check:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc


# B. Session binlog disable does not log even empty XA-prepare
#    Therefore XA-complete should be also run in sql_log_bin-OFF environment.

--let $db=test
--let $xid=skip_binlog
--let $query=insert into t2 values(1)
--connect (master_$xid, 127.0.0.1,root,,$db,$MASTER_MYPORT,)
set @@session.sql_log_bin = OFF;
--eval xa start '$xid'
  --eval $query
--eval xa end '$xid'
--eval xa prepare '$xid'

--disconnect master_$xid
--source include/wait_until_disconnected.inc

--echo *** $xid must be in the list:
--connection master
xa recover;

# now commit it carefully to avoid binlogging as the prepare part did
set @@session.sql_log_bin = OFF;
--eval xa rollback '$xid'
set @@session.sql_log_bin = ON;
--source include/save_master_gtid.inc

--echo *** Zero must be in the list:
--connection master
xa recover;

--echo *** At the end of skip_log_binb section gtid list has 0 more:
flush logs;
--let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc

#
# sync slave successfully to prove its consistency
#
--connection slave
source include/sync_with_master_gtid.inc;


connection master;
--eval drop database test_ign
drop table t1, t2, t3, tm;

--echo #
--echo # MDEV-26682 slave lock timeout with XA and gap locks
--echo #
create table t1 (a int primary key, b int unique) engine=innodb;
insert t1 values (1,1),(3,3),(5,5);
sync_slave_with_master;

# set a strong isolation level to keep the read view below.
# alternatively a long-running select can do that too even in read-committed
set session tx_isolation='repeatable-read';
start transaction;
# opens a read view to disable purge on the slave
select * from t1;

connect m2, localhost, root;
# now, delete a value, purge it on the master, but not on the slave
delete from t1 where a=3;
xa start 'x1';
# this sets a gap lock on <3>, when it exists (so, on the slave)
update t1 set b=3 where a=5;
xa end 'x1';
xa prepare 'x1';

connect m3, localhost, root;
# and this tries to insert straight into the locked gap
insert t1 values (2, 2);

echo -->slave;
sync_slave_with_master;
commit;
select * from t1;

connection m2;
xa rollback 'x1';
disconnect m2;
disconnect m3;

connection master;

drop table t1;

create table t1 (id int auto_increment primary key, c1 int not null unique)
engine=innodb;

create table t2 (id int auto_increment primary key, c1 int not null,
foreign key(c1) references t1(c1), unique key(c1)) engine=innodb;

insert t1 values (869,1), (871,3), (873,4), (872,5), (870,6), (877,7);
insert t2 values (795,6), (800,7);

xa start '1';
update t2 set id = 9, c1 = 5 where c1 in (null, null, null, null, null, 7, 3);

connect con1, localhost,root;
xa start '2';
delete from t1 where c1 like '3%';
xa end '2';
xa prepare '2';

connection master;
xa end '1';
xa prepare '1';

echo ->slave;

sync_slave_with_master;

connection slave;
source include/sync_with_master_gtid.inc;

connection con1;
xa commit '2';
disconnect con1;

connection master;
xa commit '1';
drop table t2, t1;
