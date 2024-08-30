--source include/have_partition.inc

--disable_warnings
drop table if exists t1,t3;
--enable_warnings

--echo #
--echo # MDEV-20611: MRR scan over partitioned InnoDB table produces "Out of memory" error
--echo #
create table t1(a int);
insert into t1 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

set @tmp=@@default_storage_engine;
eval set default_storage_engine=$engine_type;

create table t3 (
  ID bigint(20) NOT NULL AUTO_INCREMENT,
  part_id int,
  key_col int,
  col2 int,
  key(key_col),
  PRIMARY KEY (ID,part_id)
) PARTITION BY RANGE (part_id)
(PARTITION p1 VALUES LESS THAN (3),
 PARTITION p2 VALUES LESS THAN (7),
 PARTITION p3 VALUES LESS THAN (10)
);

show create table t3;
set default_storage_engine= @tmp;

insert into t3 select 
  A.a+10*B.a,
  A.a,
  B.a,
  123456
from t1 A, t1 B;

set @save_optimizer_switch=@@optimizer_switch;

set optimizer_switch='mrr=on';
--replace_column 9 #
explain 
select * from t3 force index (key_col) where key_col < 3;
select * from t3 force index (key_col) where key_col < 3;

set optimizer_switch=@save_optimizer_switch;

drop table t1,t3;

--echo #
--echo # MDEV-21544: partitioned table is joined with BKA+MRR
--echo #

set @save_join_cache_level=@@join_cache_level;
set @save_optimizer_switch=@@optimizer_switch;

create table t0 (
  tp int, a int, b int not null, c varchar(12), index idx (a,b)
);
insert into t0 values
(1,3,30,'yyzy'), (1,3,30,'yxxyy'), (1,3,30,'yyxy'), (1,3,30,'xxyy'),
(1,3,30,'yyxz'), (1,3,30,'yyzz'), (1,3,30,'xxyzy'), (1,3,30,'yyyy'),
(1,3,30,'yzzy'), (1,93,30,'zzzy'),
(2,3,40,'yxx'), (2,4,40,'xx'), (2,3,10,'zxz'),
(2,3,40,'yyx'), (2,4,40,'xxx'), (2,3,10,'zyyz'),
(2,3,40,'xzzzz'), (2,4,40,'yyyxx'), (2,3,10,'zyz'),
(2,3,40,'xxx'), (2,4,40,'xx'), (2,3,10,'zzz'),
(2,3,40,'yyxzx'), (2,4,40,'xyx'), (2,3,10,'xzz'),
(2,3,40,'xxxzz'), (2,4,40,'xxz'), (2,3,10,'zzzy'),
(2,3,40,'zzxxx'), (2,4,40,'zxx'), (2,3,10,'yzzz'),
(2,3,40,'xyyxx'), (2,4,40,'xzzzx'), (2,3,10,'zzxxz'),
(2,3,40,'yzxxx'), (2,4,40,'xxzy'), (2,3,10,'zzzyx'),
(2,93,40,'xzx'), (2,94,40,'xz'), (2,93,10,'zyyyz'),
(3,4,30,'yx'), (3,4,30,'yyxxx'), (3,4,30,'zzyy'), (3,4,30,'zxyy'),
(3,4,30,'xxyy'), (3,4,30,'yyzx'),  (3,4,30,'zyyy'), (3,4,30,'yzy'),
(3,4,30,'zzzyy'), (3,94,30,'yyz');

create table t1 (
  tp int, a int, b int not null, c varchar(12), index idx (a,b)
)
partition by list (tp)
( partition p1 values in (1),
  partition p2 values in (2),
  partition p3 values in (3));
insert into t1 select * from t0;

# tables t0 and t1 contain the same set of records.

create table t2 (a int, index idx(a));
insert into t2 values
(1), (2), (3), (4), (5);
insert into t2 select a+10 from t2;
insert into t2 select a+20 from t2;

analyze table t0,t1,t2;

set join_cache_level=6;
set optimizer_switch='mrr=on';

let $q1=
select * from t0,t2 where t2.a in (3,4) and t0.a=t2.a and (t0.b / 10) = t2.a-1;
eval explain extended $q1;
eval $q1;

let $q2=
select * from t1,t2 where t2.a in (3,4) and t1.a=t2.a and (t1.b / 10) = t2.a-1;
eval explain extended $q2;
eval $q2;

let $q1=
select * from t0,t2 where t2.a in (3,4) and t0.a=t2.a and (t0.b / 10) = 4;
eval explain extended $q1;
eval $q1;

let $q2=
select * from t1,t2 where t2.a in (3,4) and t1.a=t2.a and (t1.b / 10) = 4;
eval explain extended $q2;
eval $q2;

insert into t2 values
(3), (4), (5);
analyze table t2;

let $q1=
select * from t2 left join t0 on t2.a=t0.a where t2.a in (3,4) and t0.b is null;
eval explain extended $q1;
eval $q1;

let $q2=
select * from t2 left join t1 on t2.a=t1.a where t2.a in (3,4) and t1.b is null;
eval explain extended $q2;
eval $q2;

set join_cache_level=@save_join_cache_level;
set optimizer_switch=@save_optimizer_switch;

drop table t0,t1,t2;
