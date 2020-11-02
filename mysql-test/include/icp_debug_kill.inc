--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/count_sessions.inc

--disable_warnings
drop table if exists t0,t1,t2;
--enable_warnings

create table t0(a int primary key);
insert into t0 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

create table t1(a int primary key);
insert into t1 select A.a + B.a* 10 + C.a * 100 from t0 A, t0 B, t0 C;

create table t2 (
  kp1 int,
  kp2 int,
  col char(100),
  key(kp1, kp2)
);
insert into t2 select a, a, a from t1;

select engine from information_schema.tables
where table_schema=database() and table_name='t2';

explain 
select * from t2 where kp1 between 10 and 20 and kp2 +1 >100;

let $target_id= `select connection_id()`;

set debug_sync='handler_index_cond_check SIGNAL at_icp_check WAIT_FOR go';
send
select * from t2 where kp1 between 10 and 20 and kp2 +1 >100;

connect (con1, localhost, root,,);
connection con1;
set debug_sync='now WAIT_FOR at_icp_check';
evalp kill query $target_id;
set debug_sync='now SIGNAL go';

connection default;

--error ER_QUERY_INTERRUPTED
reap;
set debug_sync='RESET';

disconnect con1;
drop table t0,t1,t2;
--source include/wait_until_count_sessions.inc

