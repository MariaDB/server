--echo #
--echo # Update a with value from subquery on the same table,
--echo # no search clause. ALL access
--echo #

let $q=update t1 set c1=(select a.c3 from t1 a where a.c3 = t1.c3);
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update with search clause on the same table
--echo #

let $q=update t1 set c1=10 where c1 <2 and
   exists (select 'X' from t1 a where a.c1 = t1.c1);
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update via RANGE or INDEX access if an index or
--echo # a primary key exists
--echo #

let $q=update t1 set c1=c1+10
   where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
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

let $q=update t1 set c1=c1+10
   where exists (select 'X' from t1 a where a.c2 = t1.c2)
      and c2 >= 3 order by c2;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update using a view in subquery
--echo #

let $q=update t1 set c1=c1 +(select max(a.c2)
   from v1 a where a.c1 = t1.c1);
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update throw a view
--echo #

let $q=update v1 set c1=c1 + (select max(a.c2)
   from t1 a where a.c1 = v1.c1) +10 where c3 > 3;
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

let $q=update v1 set c1=c1 + 1 where c1 <2 and
   exists (select 'X' from v1 a where a.c1 = v1.c1);
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

let $q=update v1
   set c1=(select max(a.c1)+10 from v1 a where a.c1 = v1.c1)
      where c1 <10 and exists (select 'X' from v1 a where a.c2 = v1.c2);
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Update of the index or primary key (c3)
--echo #

let $q=update t1 set c3=c3+10
   where c2 in (select distinct a.c2 from t1 a where t1.c1=a.c1);
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # update with a limit
--echo #

let $q=update t1
         set c1=(select a.c3 from t1 a where a.c3 = t1.c3) limit 2;
eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # update with a limit and an order by
--echo #

let $q=update t1 set c1=(select a.c3 from t1 a where a.c3 = t1.c3)
   order by c3 desc limit 2;
#eval explain $q;
--enable_info ONCE
eval $q;
select concat(old_c1,'->',c1),c3,
   case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
truncate table t1;
insert into t1 select * from tmp;
