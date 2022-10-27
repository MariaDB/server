# include/index_merge_2sweeps.inc
#
# 2-sweeps read Index_merge test
#
# Last update:
# 2006-08-02 ML test refactored
#               old name was index_merge_innodb2.test
#               main code went into include/index_merge_2sweeps.inc
#
--source include/have_sequence.inc

--echo #---------------- 2-sweeps read Index merge test 2 -------------------------------

create table t1 (
  pk int primary key,
  key1 int,
  key2 int,
  filler char(200),
  filler2 char(200),
  index(key1),
  index(key2)
);

insert into t1 select seq, seq, seq, 'filler-data', 'filler-data-2'
from seq_1000_to_1;

select * from t1 where (key1 >= 2 and key1 <= 10) or (pk >= 4 and pk <=8 );

set @maxv=1000;

select * from t1 where
  (pk < 5) or (pk > 10 and pk < 15) or (pk >= 50 and pk < 55 ) or (pk > @maxv-10)
  or key1=18 or key1=60;

select * from t1 where
  (pk < 5) or (pk > 10 and pk < 15) or (pk >= 50 and pk < 55 ) or (pk > @maxv-10)
  or key1 < 3 or key1 > @maxv-11;

select * from t1 where
  (pk < 5) or (pk > 10 and pk < 15) or (pk >= 50 and pk < 55 ) or (pk > @maxv-10)
  or
  (key1 < 5) or (key1 > 10 and key1 < 15) or (key1 >= 50 and key1 < 55 ) or (key1 > @maxv-10);

select * from t1 where
  (pk > 10 and pk < 15) or (pk >= 50 and pk < 55 )
  or
  (key1 < 5) or (key1 > @maxv-10);

drop table t1;
