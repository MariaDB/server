--echo # Table-level hint
explain select /*+ no_bnl(t2@qb__dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # QB-level hint
explain select /*+ no_bnl(@qb__dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # Index-level hints
--echo # Without the hint 'range' index access would be chosen
explain select /*+ no_index(t1@`qb__T`)*/ * from
  (select * from t1 where a < 3) T;

--echo # Without the hint 'range' index access would be chosen
explain select /*+ no_range_optimization(t1@qb__t1)*/ *  from
  (select * from t1 where a > 100 and a < 120) as t1;

--echo # Regular and derived tables share same name but the hint is applied correctly
explain select /*+ index(t1@qb__t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as T1;

explain select /*+ no_index(t1@qb__t2 idx_a) index(t1@qb__t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as T1, (select * from t1 where a < 5) as T2;

explain select /*+ no_index(t1@qb__t1 idx_a, idx_ab)*/ * from
  (select * from t1 where a < 3) as T1, (select * from t1 where a < 5) as T2;

--echo # Nested derived tables
explain select /*+ no_index(t1@qb__dt1)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

explain select /*+ no_index(t1@qb__DT2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

--echo # Both hints are applied
explain select /*+ no_index(t1@qb__dt1) no_index(t1@qb__dt2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

--echo # Nested derived tables with ambiguous names
explain select * from
  (select count(*) from t1, (select * from t1 where a < 5) T1) as T1;

--echo # Warning on ambiguous query block name, hint is ignored
explain select /*+ no_index(t1@qb__t1)*/* from
  (select count(*) from t1, (select * from t1 where a < 5) T1) as T1;

--echo # The hint cannot be applied to a derived table with UNION
explain select /*+ no_index(t2@qb__t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as T1;

explain select /*+ no_index(t1@qb__t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as T1;
