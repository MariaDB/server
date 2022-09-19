create table t1 (c1 integer, c2 integer, c3 integer);

insert into t1(c1,c2,c3)
        values (1,1,1),(1,2,2),(1,3,3),
               (2,1,4),(2,2,5),(2,3,6),
               (2,4,7),(2,5,8);
insert into t1 select c1+10,c2,c3+10 from t1;
insert into t1 select c1+20,c2+1,c3+20 from t1;
analyze table t1 persistent for all;

create view v1 as select * from t1 where c2=2;

--echo #######################################
--echo #        Test without any index       #
--echo #######################################
--source include/delete_use_source_cases_memory.inc

--echo #######################################
--echo #          Test with an index         #
--echo #######################################
create index t1_c2 on t1 (c2,c1);
--source include/delete_use_source_cases_memory.inc

--echo #######################################
--echo #       Test with a primary key       #
--echo #######################################
drop index t1_c2 on t1;
alter table t1 add primary key (c3);
--source include/delete_use_source_cases_memory.inc

drop view v1;
drop table t1;

