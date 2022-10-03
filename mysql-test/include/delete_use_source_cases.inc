--echo #
--echo # Delete with value from subquery on the same table, no search clause. ALL access
--echo #
let $q = delete from t1
   where c1=(select a.c3
             from t1 a
            where a.c3 = t1.c3);
eval explain $q;
--enable_info ONCE
eval $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,1,1);

--echo #
--echo # Delete with search clause on the same table
--echo #
let $q = delete from t1
   where c1 <2
   and exists (select 'X'
                 from t1 a
                where a.c1 = t1.c2);
eval explain $q;
eval analyze $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,1,1);
insert into t1(c1,c2,c3) values (1,2,2);

--echo #
--echo # Delete via RANGE or INDEX access if an index or a primary key exists
--echo #
let $q = delete from t1 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
eval explain $q;
--enable_info ONCE
eval $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,3,3);
insert into t1(c1,c2,c3) values (2,3,6);
insert into t1(c1,c2,c3) values (2,4,7);
insert into t1(c1,c2,c3) values (2,5,8);

--echo #
--echo # Delete with order by
--echo #
let $q = delete from t1 where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3 order by c2;
eval explain $q;
eval analyze $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,3,3);
insert into t1(c1,c2,c3) values (2,3,6);
insert into t1(c1,c2,c3) values (2,4,7);
insert into t1(c1,c2,c3) values (2,5,8);

--echo #
--echo # Delete using the index or primary key (c3)
--echo #
let $q = delete from t1 where c3 in (select distinct a.c1 from t1 a where t1.c2=a.c2);
eval explain $q;
--enable_info ONCE
eval $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,1,1);
insert into t1(c1,c2,c3) values (1,2,2);

--echo #
--echo # Delete with a limit - can be delete
--echo #
let $q = delete from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c2 = t1.c3)
    limit 1;
eval explain $q;
eval analyze $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,1,1);

--echo #
--echo # Delete with a limit and an order by
--echo #

 select * from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c3 = t1.c3)
  order by c3 desc;
let $q = delete from t1 
    where c1 in (select a.c2
                    from t1 a
                    where a.c3 = t1.c3)
  order by c3 desc limit 1;
eval explain $q;
--enable_info ONCE
eval $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (2,2,5);

if($with_view){

--echo #
--echo Delete using a view in subquery
--echo #
let $q = delete from t1 
    where t1.c2 in ( select max(a.c2)
                        from v1 a
                        where a.c1 = t1.c1) ;
eval explain $q;
eval analyze $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,2,2);
insert into t1(c1,c2,c3) values (2,2,5);


--echo #
--echo # Delete throw a view
--echo #
let $q = delete from v1
   where v1.c1 in (select max(a.c1)
             from t1 a
            where a.c2 = v1.c2) 
        and c3 > 3;
eval explain $q;
--enable_info ONCE
eval $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (2,2,5);

--echo #
--echo # Delete through  a view and using the view in subquery
--echo #
let $q = delete from v1 
   where v1.c2 in (select max(a.c2)
             from t1 a
            where a.c3 = v1.c3) 
    and c1 < 10 
    and exists (select 'X'
                 from v1 a
                where a.c1 = v1.c1);
eval explain $q;
eval analyze $q;
select * from t1 order by c3;
insert into t1(c1,c2,c3) values (1,2,2);
insert into t1(c1,c2,c3) values (2,2,5);
}
