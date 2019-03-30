#
# This "body" file checks general properties of XA transaction replication
# as of MDEV-7974.
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
SET pseudo_slave_mode=1;
create table t2 (a int) engine=InnoDB;
xa start 't';
insert into t1 values (5, 6);
xa end 't';
xa prepare 't';
xa start 's';
insert into t2 values (0);
xa end 's';
xa prepare 's';

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

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;
let $diff_tables= master:t2, slave:t2;
source include/diff_tables.inc;

connection master;
drop table t1, t2;
