--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_sequence.inc
--source include/count_sessions.inc

--echo #
--echo # MDEV-22761 KILL QUERY during rowid_filter, crashes
--echo #

create table t2(a int);
insert into t2 select * from seq_0_to_99;

# 10K rows 
CREATE TABLE t3 (
  key1 int ,
  key2 int,
  filler varchar(255),
  KEY (key1),
  KEY (key2)
);
select engine from information_schema.tables
where table_schema=database() and table_name='t3';

insert into t3
select
  A.seq,
  B.seq,
  'filler-data-filler-data'
from seq_0_to_99 A, seq_0_to_99 B;

analyze table t2,t3;

explain
select * from t2, t3
where 
  t3.key1=t2.a and t3.key2 in (2,3);

let $target_id= `select connection_id()`;

set debug_sync='handler_rowid_filter_check SIGNAL at_rowid_filter_check WAIT_FOR go';
send
select * from t2, t3
where 
  t3.key1=t2.a and t3.key2 in (2,3);

connect (con1, localhost, root,,);
set debug_sync='now WAIT_FOR at_rowid_filter_check';
evalp kill query $target_id;
set debug_sync='now SIGNAL go';

connection default;
disconnect con1;

--error ER_QUERY_INTERRUPTED
reap;
set debug_sync='RESET';

drop table t2,t3;
--source include/wait_until_count_sessions.inc
