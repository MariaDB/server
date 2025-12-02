--echo # ======================================
--echo # Views
--echo #
--echo # select /* The name of the current query block is @SEL_1 */ * from v1;
--echo #
--echo # Addressing a view having one query block.
--echo # QB_NAME(qb_v1, v1) means: inner query block of the view `v1`,
--echo # which is present in the same query block as the hint, gets the name `qb_v1`.
--echo # This name can be used in other hints.
explain extended
  select /*+ qb_name(qb_v1, v1) no_index(t1@qb_v1)*/* from v1;

--echo # Equivalent to the above but specifying @SEL_1 explicitly.
--echo # QB_NAME(qb_v1, v1@sel_1) means: inner query block of view `v1`,
--echo # which is present in SELECT#1 of the current query block, gets the name `qb_v1`.
explain extended
  select /*+ qb_name(qb_v1, v1@sel_1) no_index(t1@qb_v1 idx_a)*/* from v1;

--echo # Equivalent to the above but also specifying @SEL_1 of the view.
--echo # QB_NAME(qb_v1, v1@sel_1 .@sel_1) means: SELECT#1 of view `v1`,
--echo # which is present in SELECT#1 of the current query block, gets the name `qb_v1`.
explain extended
  select /*+ qb_name(qb_v1, v1@sel_1 .@sel_1) no_index(t1@qb_v1 idx_ab)*/* from v1;

--echo #
--echo # The case when a particular view is used in more than one query block.
--echo #   select /* Name of current query block is @SEL_1 */ * from v1
--echo #   join
--echo #   (select /* Name of current query block is @SEL_2 */ * from v1) vvv1;
--echo # The first query block of view v1 can be declared as
--echo #   QB_NAME(v1_1, v1@SEL_1 .@SEL_1),
--echo # and the second query block of the view v1 can be declared as
--echo #   QB_NAME(v1_2, v1@SEL_1 .@SEL_2).
--echo #
--echo # By default, range access is used for both `t1`'s in the statement below. 
explain extended
  select * from v1 join (select * from v1) vvv1;

--echo # Disable index access for t1 from the second occurence of view v1:
explain extended
  select /*+ qb_name(v1_2, v1@SEL_2 .@SEL_1) no_index(t1@v1_2)*/ *
    from v1 join (select * from v1) vvv1;

--echo # Disable index access for t1 from both occurences of view v1:
explain extended
  select /*+ qb_name(v1_1, v1@SEL_1) qb_name(v1_2, v1@SEL_2 .@SEL_1)
      no_index(t1@v1_1) no_index(t1@v1_2)*/ *
    from v1 join (select * from v1) vvv1;

--echo #
--echo # The case when a particular view has more than one query block.
--echo # create view v2 as
--echo #   select * from t1 join /* Name of this query block is @SEL_1 */
--echo #   (
--echo #     select count(*) from t1 join v1 /* Name of this query block is @SEL_2 */
--echo #   ) tt;
--echo # The first query block of view v2 can be declared as
--echo # QB_NAME(v2_1, v2@SEL_1 .@SEL_1), and the second query block can be
--echo # declared as QB_NAME(v2_2, v2@SEL_1 .@SEL_2).
--echo #
--echo # See the default execution plan:
explain extended select * from v2;

--echo # Disable index access for t1 from the second query block of view v2:
explain extended
  select /*+ qb_name(v2_2, v2@SEL_1 .@SEL_2) no_index(t1@v2_2)*/* from v2;

--echo # Disable index access for `t1` from view `v1` used in
--echo # the first query block of view `v2`:
explain extended
  select /*+ qb_name(v2_v1, v2@SEL_1 .v1@SEL_2) no_index(t1@v2_v1)*/* from v2;

--echo # Equivalent to the above but specifying @SEL_1 explicitly:
explain extended
  select /*+ qb_name(v2_v1_sel1, v2@SEL_1 .v1@SEL_2 .@SEL_1)
             no_index(t1@v2_v1_sel1)*/ * from v2;

--echo # Disable index access for `t1` tables from views `v1` and `v2`
explain extended
  select /*+ qb_name(v2_v1, v2@SEL_1 .v1@SEL_2) no_index(t1@v2_v1)
             qb_name(v2_2, v2@SEL_1 .@SEL_2) no_index(t1@v2_2) */ * from v2;

--echo # ======================================
--echo # Views with UNION
--echo #
--echo # Default execution plan:
explain extended select * from v3;

--echo # `v3` in QB path corresponds to @SEL_1:
explain extended select /*+ qb_name(qb_v3, v3) no_index(t1@qb_v3)*/* from v3;

--echo # Addressing @SEL_1 of `v3` explicitly:
explain extended
  select /*+ qb_name(qb_v3_sel1, v3.@sel_1) no_index(t1@qb_v3_sel1)*/* from v3;

--echo # Addressing @SEL_2 of `v3` explicitly:
explain extended
  select /*+ qb_name(qb_v3_sel2, v3.@sel_2) no_index(t1@qb_v3_sel2)*/* from v3;


--echo # ======================================
--echo # Derived tables
--echo #
--echo # QB_NAME(qb_dt, dt) means: inner query block of derived table `dt`,
--echo # which is present in the same query block as the hint, gets the name `qb_dt`.
--echo # This name can be used in other hints.
explain extended select /*+ qb_name(qb_dt, dt) no_index(t1@qb_dt)*/* from
  (select * from t1 where a < 10) dt;

--echo # QB_NAME(qb_dt, dt@sel_1) means: inner query block of derived table `dt`,
--echo # which is present in SELECT#1 of the current query block, gets the name `qb_dt`.
explain extended select /*+ qb_name(qb_dt, dt@sel_1) no_index(t1@qb_dt)*/* from
  (select * from t1 where a < 10) dt;

--echo # QB_NAME(qb_dt, dt@sel_1 .@sel_1) means: SELECT#1 of derived table `dt`,
--echo # which is present in SELECT#1 of the current query block, gets the name `qb_dt`.
explain extended select /*+ qb_name(qb_dt, dt@sel_1 .@sel_1) no_index(t1@qb_dt)*/* from
  (select * from t1 where a < 10) dt;

--echo # ======================================
--echo # Derived tables with UNION
--echo #
--echo # Default execution plan:
explain extended
select * from (select * from t1 where a < 10 union select * from t1 where a > 90) dt;

--echo # `dt` in QB path corresponds to @SEL_1:
explain extended
select /*+ qb_name(qb_dt, dt) no_index(t1@qb_dt)*/* from
  (select * from t1 where a < 10 union select * from t1 where a > 90) dt;

--echo # Addressing @SEL_1 of `dt` explicitly:
explain extended
select /*+ qb_name(qb_dt_sel1, dt.@sel_1) no_index(t1@qb_dt_sel1)*/* from
  (select * from t1 where a < 10 union select * from t1 where a > 90) dt;

--echo # Addressing @SEL_2 of `dt` explicitly:
explain extended
select /*+ qb_name(qb_dt_sel2, dt.@sel_2) no_index(t1@qb_dt_sel2)*/* from
  (select * from t1 where a < 10 union select * from t1 where a > 90) dt;

--echo # ======================================
--echo # Mix of views and derived tables
--echo #
explain extended
select /*+ qb_name(dt1_v1_1, dt1 .v1 .@SEL_1) no_index(t1@dt1_v1_1)*/ *
  from v1 join (select * from v1) dt1;

--echo # More complicated query. Default execution plan:
explain extended
select v1.* from v1 join (select v1.* from v1 join (select * from v2) dt2) dt1;

explain extended
select /*+ qb_name(dt1_v1_1, dt1 .v1 .@SEL_1) no_index(t1@dt1_v1_1)*/ v1.*
  from v1 join (select v1.* from v1 join (select * from v2) dt2) dt1;

explain extended
select /*+ qb_name(dt2_dt1_v1_1, dt1 .dt2 .v2 .@SEL_2)
           no_index(t1@dt2_dt1_v1_1)*/ v1.*
  from v1 join (select v1.* from v1 join (select * from v2) dt2) dt1;

--echo # ======================================
--echo # CTEs
--echo #
--echo # Default execution plan:
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select * from cte;

--echo # Disable index access for t1 in CTE
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ qb_name(qb_cte, cte) no_index(t1@qb_cte)*/ * from cte;

--echo # Disable index access for t1 in dt1 of CTE
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ qb_name(qb_cte_dt1, cte .dt1) no_index(t1@qb_cte_dt1)*/ * from cte;

--echo # Disable index access for both t1's
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ qb_name(qb_cte, cte) no_index(t1@qb_cte)
           qb_name(qb_cte_dt1, cte .dt1) no_index(t1@qb_cte_dt1)*/ * from cte;

--echo # Multiple references to a CTE in a query.
--echo # Default execution plan:
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select * from cte join cte cte1;

--echo # Disable index access for t1 in `cte`
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ qb_name(qb_cte, cte) no_index(t1@qb_cte)*/ * from cte join cte as cte1;

--echo # Disable index access for t1 in `cte1`
explain extended
with cte as (select count(*) from t1, (select * from t1 where a < 5) dt1)
select /*+ qb_name(qb_cte1, cte1) no_index(t1@qb_cte1)*/ * from cte join cte as cte1;


--echo # ======================================
--echo # Wrong paths generate warnings
--echo #
--disable_ps_protocol
# PS protocol is disabled because the warning is genererated only during
# PREPARE step of a statement. When the QB name cannot be resolved the hint
# is discarded, so it is not present during EXECUTE step.

explain extended
  select /*+ qb_name(`qb_v1`, `v2`)*/* from v1;

--echo # Wrong select number:
explain extended
  select /*+ qb_name(qb_v1, `v2`@`sel_2`)*/* from v1;

explain extended
  select /*+ qb_name(qb_v1, v2@sel_1 .@sel_2)*/* from v1;

--echo # Attempting to reference a regular table as a query block:
explain extended
  select /*+ qb_name(qb_v1, dt .t1)*/* from (select t1.* from t1 join v1) dt;

--echo # Wrong view name inside a derived table:
explain extended
  select /*+ qb_name(qb_v1, dt .v2)*/* from (select t1.* from t1 join v1) dt;

--echo # Wrong select number:
explain extended
select /*+ qb_name(qb_v1, dt .v1@sel_2)*/* from (select t1.* from t1 join v1) dt;

explain extended
select /*+ qb_name(qb_v1, dt .v1@sel_1 .@sel_2)*/* from
  (select t1.* from t1 join v1) dt;

--echo # Wrong select number syntax:
explain extended
  select /*+ qb_name(qb_v1, `v1`@`lex_2`)*/* from v1;

explain extended
  select /*+ qb_name(qb_v1, v1 .lex_2)*/* from v1;

--enable_ps_protocol