let $prior_set_lwt = `select concat('set @prior_lock_wait_timeout = @@',
  '$engine', '_lock_wait_timeout;')`;
let $prior_set_dld = `select concat('set @prior_deadlock_detect = @@',
  '$engine', '_deadlock_detect;')`;
let $global_dld = `select concat('set global ', '$engine',
  '_deadlock_detect = on;')`;
let $global_lwt = `select concat('set global ', '$engine',
  '_lock_wait_timeout = 100000;')`;
eval $prior_set_lwt $prior_set_dld $global_dld $global_lwt;

--source include/count_sessions.inc
connect (con1,localhost,root,,);
let $con1= `SELECT CONNECTION_ID()`;

connect (con2,localhost,root,,);
let $con2= `SELECT CONNECTION_ID()`;

connection default;
eval create table t (i int primary key) engine=$engine;
insert into t values (1), (2), (3);

--echo #
--echo # The following is disabled due:
--echo #   MDEV-13404: MyRocks upstream uses I_S.table_statistics.row_lock_deadlocks, should we import?
--echo #
--disable_parsing
--source include/simple_deadlock.inc
connection default;
select row_lock_deadlocks from information_schema.table_statistics where
table_name = "t";

select row_lock_deadlocks from information_schema.table_statistics where
table_name = "t";
--source include/simple_deadlock.inc
connection default;
select row_lock_deadlocks from information_schema.table_statistics where
table_name = "t";

select row_lock_deadlocks from information_schema.table_statistics where
table_name = "t";
--enable_parsing

disconnect con1;
disconnect con2;

let $restore_lwt = `select concat('set global ', '$engine',
  '_lock_wait_timeout = @prior_lock_wait_timeout;')`;
let $restore_dld = `select concat('set global ', '$engine',
  '_deadlock_detect = @prior_deadlock_detect;')`;
eval $restore_lwt $restore_dld;
drop table t;
--source include/wait_until_count_sessions.inc
