# Include to test update with same table as source and target

--echo #
--echo # Update a with value from subquery on the same table, no search clause. ALL access
--echo #

start transaction;
--enable_info ONCE
update t1
   set c1=(select a.c3
             from t1 a
            where a.c3 = t1.c3);
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1 ;
rollback;

--echo #
--echo # Update with search clause on the same table
--echo #

start transaction;
--enable_info ONCE
update t1
   set c1=10
 where c1 <2
   and exists (select 'X'
                 from t1 a
                where a.c1 = t1.c1);
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1 ;
rollback;

--echo #
--echo # Update via RANGE or INDEX access if an index or a primary key exists
--echo #

explain update t1 set c1=0 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 > 3;
start transaction;
--enable_info ONCE
update t1 set c1=c1+10 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo # Update with order by
--echo #

start transaction;
update t1 set c1=c1+10 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3 order by c2;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo Update using a view in subquery
--echo #

start transaction;
--enable_info ONCE
update t1
   set c1=c1 +(select max(a.c2)
             from v1 a
            where a.c1 = t1.c1) ;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo # Update throw a view
--echo #

start transaction;
--enable_info ONCE
update v1
   set c1=c1 + (select max(a.c2)
             from t1 a
            where a.c1 = v1.c1) +10
where c3 > 3;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo # Update through a view and using the view in subquery
--echo #

start transaction;
--enable_info ONCE
update v1
   set c1=c1 + 1
 where c1 <2
   and exists (select 'X'
                 from v1 a
                where a.c1 = v1.c1);
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo # Update through  a view and using the view in subquery
--echo #

start transaction;
--enable_info ONCE
update v1
   set c1=(select max(a.c1)+10
             from v1 a
            where a.c1 = v1.c1)
 where c1 <10
   and exists (select 'X'
                 from v1 a
                where a.c2 = v1.c2);
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1;
rollback;

--echo #
--echo # Update of the index or primary key (c3)
--echo #

start transaction;
explain update t1 set c3=c3+10 where c2 in (select distinct a.c2 from t1 a where t1.c1=a.c1);
--enable_info ONCE
update t1 set c3=c3+10 where c2 in (select distinct a.c2 from t1 a where t1.c1=a.c1);
select c3 from t1;
rollback;

--echo #
--echo # update with a limit
--echo #

start transaction;
--enable_info ONCE
update t1
   set c1=(select a.c3
             from t1 a
            where a.c3 = t1.c3)
    limit 2;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1 ;
rollback;

--echo #
--echo # update with a limit and an order by
--echo #

start transaction;
--enable_info ONCE
update t1
   set c1=(select a.c3
             from t1 a
            where a.c3 = t1.c3)
  order by c3 desc limit 2;
select concat(old_c1,'->',c1),c3, case when c1 != old_c1 then '*' else ' ' end "Changed" from t1 ;
rollback;
