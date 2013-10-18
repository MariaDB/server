set @@optimizer_switch = "static_cond_pushdown=on";
set @@use_stat_tables='preferably';
set @@optimizer_use_condition_selectivity=4;

drop table if exists t0, t1 ,t2, t3, sq;

create table t0 (z1 char(64));
create table t1 (a1 char(64), a2 char(64), a3 char(64), a4 char(64));
create table t2 (b1 char(64), b2 char(64), b3 char(64), b4 char(64));
create table t3 (c1 char(64), c2 char(64), c3 char(64), c4 char(64));
create table sq (s1 char(64));

insert into t0 values ('11'),('22'),('33'),('44'),('55'), ('66'),('77'),('88'),('99'),('10');
insert into t1 select z1, z1, z1, z1 from t0;
insert into t2 select z1, z1, z1, z1 from t0;
insert into t3 select z1, z1, z1, z1 from t0;
insert into sq select z1 from t0;

analyze table t1,t2,t3;

--echo veryfy estimated predicate selectivities
explain extended
SELECT count(*) from t2 where b2 < '6' and b3 < '4' and b4 < '2';

--echo uncorrelated SQ, 2-way join
explain extended
SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > 0);

SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > 0);

--echo correlated SQ, 2-way join
explain extended
SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > a1);

SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > a1);

--echo 2 correlated SQ, 2-way join
explain extended
SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > a1) and
      b2 < (select count(*) from sq where s1 > b1);

SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      a2 < (select count(*) from sq where s1 > a1) and
      b2 < (select count(*) from sq where s1 > b1);

--echo 2 correlated SQ, , 2-way join, one SQ inside an OR
explain extended
SELECT count(*)
FROM t1 straight_join t2
WHERE a1 = b1 and b2 < '6' and b3 < '4' and b4 < '2' and
      (a2 < (select count(*) from sq where s1 > a1) or a1 > 0) and
      b2 < (select count(*) from sq where s1 > b1);

--echo uncorrelated SQ, 3-way join

--echo the number of buffer refills > partial join size of t3, move
--echo join_tab->cache_select->cond to t3
set @@join_buffer_size=128;

explain extended
SELECT count(*)
FROM t1 straight_join t2 straight_join t3
WHERE a1 = b1 and b2 = c1 and c2 < '6' and c3 < '4' and c4 < '2' and
      b2 <= (select count(*) from sq where s1 > 0);

SELECT count(*)
FROM t1 straight_join t2 straight_join t3
WHERE a1 = b1 and b2 = c1 and c2 < '6' and c3 < '4' and c4 < '2' and
      b2 <= (select count(*) from sq where s1 > 0);

--echo the number of buffer refills < partial join size of t3
set @@join_buffer_size=default;

explain extended
SELECT count(*)
FROM t1 straight_join t2 straight_join t3
WHERE a1 = b1 and b2 = c1 and c2 < '6' and c3 < '4' and c4 < '2' and
      b2 <= (select count(*) from sq where s1 > 0);

SELECT count(*)
FROM t1 straight_join t2 straight_join t3
WHERE a1 = b1 and b2 = c1 and c2 < '6' and c3 < '4' and c4 < '2' and
      b2 <= (select count(*) from sq where s1 > 0);


drop table t0, t1 ,t2, t3, sq;

set @@optimizer_switch = default;
