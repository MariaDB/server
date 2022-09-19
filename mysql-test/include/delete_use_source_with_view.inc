--echo #
--echo Delete using a view in subquery
--echo #
start transaction;
--enable_info ONCE
delete from t1 
    where t1.c2 in ( select max(a.c2)
                        from v1 a
                        where a.c1 = t1.c1) ;
select * from t1;
rollback;


--echo #
--echo # Delete throw a view
--echo #
start transaction;
--enable_info ONCE
delete from v1
   where v1.c1 in (select max(a.c1)
             from t1 a
            where a.c2 = v1.c2) 
        and c3 > 3;
select * from t1;
rollback;


--echo #
--echo # Delete through  a view and using the view in subquery
--echo #
start transaction;
--enable_info ONCE
delete from v1 
   where v1.c2 in (select max(a.c2)
             from t1 a
            where a.c3 = v1.c3) 
    and c1 < 10 
    and exists (select 'X'
                 from v1 a
                where a.c1 = v1.c1);
select * from t1;
rollback;
