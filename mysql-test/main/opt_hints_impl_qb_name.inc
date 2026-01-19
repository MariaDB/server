--echo # Table-level hint
explain extended select /*+ no_bnl(t2@dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # QB-level hint
explain extended select /*+ no_bnl(@dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # Index-level hints
--echo # Without the hint 'range' index access would be chosen
explain extended select /*+ no_index(t1@`T`)*/ * from
  (select * from t1 where a < 3) t;

--echo # Without the hint 'range' index access would be chosen
explain extended select /*+ no_range_optimization(t1@t1)*/ *  from
  (select * from t1 where a > 100 and a < 120) as t1;

--echo # Regular and derived tables share same name but the hint is applied correctly
explain extended select /*+ index(t1@t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as t1;

explain extended select /*+ no_index(t1@t2 idx_a) index(t1@t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as t1, (select * from t1 where a < 5) as t2;

explain extended select /*+ no_index(t1@t1 idx_a, idx_ab)*/ * from
  (select * from t1 where a < 3) as t1, (select * from t1 where a < 5) as t2;

--echo # Nested derived tables
explain extended select /*+ no_bnl(t1@dt2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) dt1) as dt2;

explain extended select /*+ no_index(t1@DT2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) dt1) as dt2;

--echo # Explicit QB name overrides the implicit one
explain extended select /*+ no_index(t1@dt2)*/ * from
  (select count(*) from t1, (select /*+ qb_name(dt2)*/ * from t1 where a < 5) dt1) as dt2;

--echo # Both hints are applied
explain extended select /*+ no_index(t1@dt1) no_bnl(t1@dt2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) dt1) as dt2;

--echo # Nested derived tables with ambiguous names
explain extended select * from
  (select count(*) from t1, (select * from t1 where a < 5) t1) as t1;

--echo # Warning on ambiguous query block name, hint is ignored

--disable_ps_protocol
# PS protocol is disabled because the warning is genererated only during
# PREPARE step of a statement. When the QB name cannot be resolved the hint
# is discarded, so it is not present during EXECUTE step. The same applies
# not only to implicit but also to explicit QB names.

explain extended select /*+ no_index(t1@t1)*/* from
  (select count(*) from t1, (select * from t1 where a < 5) t1) as t1;

--echo # The hint cannot be applied to a derived table with UNION
explain extended select /*+ no_index(t2@t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as t1;

explain extended select /*+ no_index(t1@t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as t1;

--echo # Test INSERT..SELECT
explain extended insert into t2 select /*+ no_bnl(t2@dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # MERGE/NO_MERGE hints are resolved early and so do not support
--echo # implicit QB names. Warning is expected
explain extended select /*+ no_merge(@dt)*/ * from
  (select * from (select t1.* from t1, t2 where t1.a > t2.a) as dt1) as dt;

--enable_ps_protocol

--echo # ======================================
--echo # Test CTEs
--echo # By default BNL and index access to t1 is used.
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select * from cte;

explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ no_bnl(t1@cte)*/ * from cte;

explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ no_bnl(@cte)*/ * from cte;

explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ no_index(t1@cte)*/ * from cte;

explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ no_index(t1@cte) no_index(t1@dt1)*/ * from cte;

explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ no_index(t1@dt1 idx_a) no_bnl(@cte)*/ * from cte;

--echo # Ambiguity: multiple occurencies of `cte`, the hint is ignored

--disable_ps_protocol
# See the comment above for why PS protocol is disabled

explain extended
with cte as (select count(*) as cnt from t1, (select * from t1 where a < 5) dt1)
select /*+ no_bnl(@cte)*/ * from cte where cnt > 10
union
select * from cte where cnt < 100;

--echo # However, if CTE occurencies have different aliases, the hint can be applied
explain extended
with cte as (select count(*) as cnt from t1, (select * from t1 where a < 5) dt1)
select /*+ no_index(t1@cte1)*/ * from cte as cte1 where cnt > 10
union
select * from cte where cnt < 100;

--enable_ps_protocol

--echo # ======================================
--echo # Test views
create view v1 as select * from t1 where a < 100;

--echo # Default execution plan
explain extended select *
  from v1, v1 as v2 where v1.a = v2.a and v1.a < 3;

explain extended
  select /*+ index(t1@`v1` idx_ab) no_index(t1@`v2`)*/ *
  from v1, v1 as v2 where v1.a = v2.a and v1.a < 3;

--echo # Nested views
create view v2 as select * from v1 where a < 300;

--echo # Default execution plan
explain extended select * from v2;

--echo # Addressing an object inside a nested view
explain extended select /*+ index(t1@`v1` idx_ab)*/ * from v2;

create view v3 as select * from t1 union select * from t2;

--echo # Unable to apply the hint to a table with UNION

--disable_ps_protocol
# See the comment above for why PS protocol is disabled

explain extended select /*+ no_index(t1@v3) */ * from v3;

--echo # Ambiguity: view `v1` appears two times - should warn and ignore hint
explain extended select /*+ index(t2@v1) */ * from v1,
   (select a from v1 where b > 5) dt;

--enable_ps_protocol

--echo # Implicit QB names are not supported inside views
create view v4 as select /*+ no_bnl(t2@dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

show create view v4;

--echo # However, a derived table inside a view can be addressed from outer query
create view v5 as select dt.a from
    t1, (select t1.* from t1, t2 where t1.a > t2.a) as dt where t1.a=dt.a;

--echo # Addressing a single table
explain extended select /*+ no_bnl(t2@dt) */* from v5;
--echo # Addressing the whole derived table
explain extended select /*+ no_bnl(@dt) */* from v5;

--echo # Derived tables inside views can be addressed by their aliases
explain extended select /*+ no_bnl(t2@dt) */ * from v4;

drop view v1, v2, v3, v4, v5;