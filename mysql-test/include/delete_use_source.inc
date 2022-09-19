--echo #
--echo # Delete with value from subquery on the same table, no search clause. ALL access
--echo #
start transaction;
--enable_info ONCE
delete from t1
   where c1=(select a.c3
             from t1 a
            where a.c3 = t1.c3);
select * from t1;
rollback;

--echo #
--echo # Delete with search clause on the same table
--echo #
start transaction;
--enable_info ONCE
delete from t1
   where c1 <2
   and exists (select 'X'
                 from t1 a
                where a.c1 = t1.c2);
select * from t1;
rollback;

--echo #
--echo # Delete via RANGE or INDEX access if an index or a primary key exists
--echo #
start transaction;
--enable_info ONCE
delete from t1 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
select * from t1;
rollback;


--echo #
--echo # Delete with order by (need fix MDEV-29428)
--echo #
start transaction;
--enable_info ONCE
delete from t1 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3 order by c2;
select * from t1;
rollback;


--echo #
--echo # Delete using the index or primary key (c3)
--echo #
start transaction;
explain delete from t1 where c3 in (select distinct a.c1 from t1 a where t1.c2=a.c2);
--enable_info ONCE
delete from t1 where c3 in (select distinct a.c1 from t1 a where t1.c2=a.c2);
select * from t1;
rollback;


--echo #
--echo # Delete with a limit - can be delete
--echo #
start transaction;
--enable_info ONCE
delete from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c3 = t1.c3)
    limit 1;
select * from t1;
rollback;

--echo #
--echo # Delete with a limit and an order by
--echo #

start transaction;
 select * from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c3 = t1.c3)
  order by c3 desc;
--enable_info ONCE
delete from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c3 = t1.c3)
  order by c3 desc limit 1;
select * from t1;
rollback;
