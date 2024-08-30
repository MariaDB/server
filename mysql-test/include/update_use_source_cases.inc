--echo #
--echo # Update with value from subquery on the same table
--echo #

analyze table t1 persistent for all;
let $q = update t1 set c1=(select a.c3 from t1 a where a.c3 = t1.c3);
eval explain select * from t1;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with EXISTS subquery over the updated table
--echo # in WHERE + possibly sargable condition
--echo #

analyze table t1 persistent for all;
let $c = c1 <2
          and exists (select 'X' from t1 a where a.c1 = t1.c1);
let $q = update t1 set c1=10 where $c;
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with EXISTS subquery over the updated table
--echo # in WHERE + non-sargable condition
--echo #

analyze table t1 persistent for all;
let $c = exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
let $q = update t1 set c1=c1+10 where $c;
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with order by
--echo #

analyze table t1 persistent for all;
let $c = exists (select 'X' from t1 a where a.c2 = t1.c2)
            and c2 >= 3 order by c2;
let $q = update t1 set c1=c1+10 where $c;
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with a reference to view in subquery
--echo # in settable value
--echo #

analyze table t1 persistent for all;
let $q = update t1 set c1=c1 +(select max(a.c2) from v1 a
                              where a.c1 = t1.c1);
eval explain select * from t1;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update view
--echo #

analyze table t1 persistent for all;
let $q = update v1 set c1=c1 + (select max(a.c2) from t1 a
                                 where a.c1 = v1.c1) +10 where c3 > 3;
eval explain select * from v1;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update view with reference to the same view in subquery
--echo #

analyze table t1 persistent for all;
let $c = c1 <2
         and exists (select 'X' from v1 a where a.c1 = v1.c1);
let $q = update v1 set c1=c1 + 1 where $c;
eval explain select * from v1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update view with EXISTS and reference to the same view in subquery
--echo #

analyze table t1 persistent for all;
let $c = c1 <10 and exists (select 'X' from v1 a where a.c2 = v1.c2);
let $q = update v1
      set c1=(select max(a.c1)+10 from v1 a where a.c1 = v1.c1)
          where $c;
eval explain select * from v1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with IN predicand over the updated table in WHERE
--echo #

analyze table t1 persistent for all;
let $c = c2 in (select distinct a.c2 from t1 a where t1.c1=a.c1);
let $q = update t1 set c3=c3+110 where $c;
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with a limit
--echo #

analyze table t1 persistent for all;
let $q = update t1
         set c1=(select a.c3 from t1 a where a.c3 = t1.c3) limit 2;
eval explain select * from t1 limit 2;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with a limit and an order by
--echo #

analyze table t1 persistent for all;
let $q=update t1
         set c1=(select a.c3 from t1 a where a.c3 = t1.c3)
            order by c3 desc limit 2;
eval explain select * from t1 order by c3 desc limit 2;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;
