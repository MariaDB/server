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

set optimizer_switch='mrr=on';
--replace_column 9 #
explain 
select * from t3 force index (key_col) where key_col < 3;
select * from t3 force index (key_col) where key_col < 3;

drop table t1,t3;

