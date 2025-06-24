--echo #
--echo # Delete with EXISTS subquery over the updated table
--echo # in WHERE + non-sargable condition
--echo #

analyze table t1 persistent for all;

let $c = exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 = 3;
eval create table tmp as select * from t1 where $c;
let $q = delete from t1 where $c;
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;

--echo #
--echo # Delete with order by
--echo #

analyze table t1 persistent for all;

let $c = exists (select 'X' from t1 a where a.c2 = t1.c2)
          and c2 >= 3 order by c2;
eval create table tmp as select * from t1 where $c;
let $q = delete from t1 where $c;
--replace_column 9 #
eval explain select * from t1 where $c;
--replace_column 9 #
eval explain $q;
--replace_column 9 #
eval analyze $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;

--echo #
--echo # Delete from view
--echo #

analyze table t1 persistent for all;

let $c = v1.c1 in
        (select max(a.c1) from t1 a where a.c2 = v1.c2) and c3 = 5;
eval create table tmp as select * from v1 where $c;
let $q = delete from v1 where $c;
eval explain select * from v1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;

--echo #
--echo # Delete from view using reference
--echo # to the same view in subquery
--echo #

analyze table t1 persistent for all;

let $c = v1.c2 in (select max(a.c2) from t1 a where a.c3 = v1.c3)
          and c1 = 2
          and exists (select 'X' from v1 a where a.c1 = v1.c1);
eval create table tmp as select * from v1 where $c;
let $q = delete from v1 where $c;
--replace_column 9 #
eval explain select * from v1 where $c;
--replace_column 9 #
eval explain $q;
--replace_column 9 #
eval analyze $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;
