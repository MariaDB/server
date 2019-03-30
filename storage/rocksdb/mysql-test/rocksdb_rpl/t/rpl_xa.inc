#
# This "body" file checks general properties of XA transaction replication
# as of MDEV-7974, including XA of mixed engine branches.
# Parameters:
# --let rpl_xa_check= SELECT ...
#
connection master;
create table ti (a int, b int) engine=innodb;
create table t1 (a int, b int) engine=rocksdb;
insert into ti values(0, 0);
insert into t1 values(0, 0);
xa start 't';
insert into ti values(1, 2);
insert into t1 values(1, 2);
xa end 't';
xa prepare 't';
xa commit 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;

xa start 't';
insert into ti values(3, 4);
insert into t1 values(3, 4);
xa end 't';
xa prepare 't';
xa rollback 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;
SET pseudo_slave_mode=1;
create table t2 (a int) engine=rocksdb;
xa start 't';
insert into ti values (5, 6);
insert into t1 values (5, 6);
xa end 't';
xa prepare 't';
xa start 's';
insert into ti values (7, 8);
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
xa recover;

connection master;
xa commit 't';
xa commit 's';
SET pseudo_slave_mode=0;

# pure rocksdb xa
xa start 'r';
insert into t1 values(7, 8);
xa end 'r';
xa prepare 'r';
xa commit 'r';


sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;
let $diff_tables= master:t2, slave:t2;
source include/diff_tables.inc;

connection master;
drop table ti, t1, t2;
