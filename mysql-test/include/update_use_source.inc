# Include to test update with same table as source and target

create table t1 (old_c1 integer,
                 old_c2 integer,
                 c1 integer,
                 c2 integer,
                 c3 integer);

create view v1 as select * from t1 where c2=2;
delimiter /;
create trigger trg_t1 before update on t1 for each row
begin
  set new.old_c1=old.c1;
  set new.old_c2=old.c2;
end;
/
delimiter ;/

insert into t1(c1,c2,c3)
        values (1,1,1), (1,2,2), (1,3,3),
               (2,1,4), (2,2,5), (2,3,6),
               (2,4,7), (2,5,8);
insert into t1 select NULL, NULL, c1+10,c2,c3+10 from t1;
insert into t1 select NULL, NULL, c1+20,c2+1,c3+20 from t1;
analyze table t1 persistent for all;

create table tmp as select * from t1;

--echo #######################################
--echo #        Test without any index       #
--echo #######################################
--source include/update_use_source_cases.inc

--echo #######################################
--echo #          Test with an index         #
--echo #######################################
create index t1_c2 on t1 (c2,c1);
analyze table t1;
--source include/update_use_source_cases.inc

--echo #######################################
--echo #       Test with a primary key       #
--echo #######################################
drop index t1_c2 on t1;
alter table t1 add primary key (c3);
analyze table t1;
--source include/update_use_source_cases.inc

--echo # Update with error "Subquery returns more than 1 row"
--error ER_SUBQUERY_NO_1_ROW
update t1 set c2=(select c2 from t1);
--sorted_result
select c1,c2,c3 from t1;

--echo # Update with error "Subquery returns more than 1 row"
--echo # and order by
--error ER_SUBQUERY_NO_1_ROW
update t1 set c2=(select c2 from t1) order by c3;
--sorted_result
select c1,c2,c3 from t1;

-- echo # Duplicate value on update a primary key
--error ER_DUP_ENTRY

update t1 set c3=0
    where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

-- echo # Duplicate value on update a primary key with ignore
--enable_info ONCE
update ignore t1 set c3=0
    where exists (select 'X' from t1 a where a.c2 = t1.c2) and c2 >= 3;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

-- echo # Duplicate value on update a primary key and limit
--error ER_DUP_ENTRY
update t1 set c3=0
    where exists (select 'X' from t1 a where a.c2 = t1.c2)
      and c2 >= 3 limit 2;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

-- echo # Duplicate value on update a primary key with ignore
-- echo # and limit
--enable_info ONCE
update ignore t1 set c3=0
    where exists (select 'X' from t1 a where a.c2 = t1.c2)
    and c2 >= 3 limit 2;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo # Update no rows found
--enable_info ONCE
update t1 set c1=10
    where c1 <2 and exists (select 'X' from t1 a where a.c1 = t1.c1 + 10);
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo # Update no rows changed
drop trigger trg_t1;
--enable_info ONCE
update t1 set c1=c1
    where c1 <2 and exists (select 'X' from t1 a where a.c1 = t1.c1);
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Check call of after trigger
--echo #

delimiter /;
create or replace trigger trg_t2 after update on t1 for each row
begin
  declare msg varchar(100);
  if (new.c3 = 5) then
    set msg=concat('in after update trigger on ',new.c3);
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = msg;
  end if;
end;
/
delimiter ;/
--error 1644

update t1 set c1=2
    where c3 in (select distinct a.c3 from t1 a where a.c1=t1.c1);
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

--echo #
--echo # Check update with order by and after trigger
--echo #

--error 1644
update t1 set c1=2
    where c3 in (select distinct a.c3 from t1 a where a.c1=t1.c1)
    order by t1.c2, t1.c1;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

drop view v1;

--echo #
--echo # Check update on view with check option
--echo #

create view v1 as select * from t1 where c2=2 with check option;

-- error 1369
update v1 set c2=3 where c1=1;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

-- error 1369
update v1 set c2=(select max(c3) from v1) where c1=1;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

update v1 set c2=(select min(va.c3) from v1 va), c1=0 where c1=1;
--sorted_result
select c1,c2,c3 from t1;
truncate table t1;
insert into t1 select * from tmp;

drop table tmp;
drop view v1;
drop table t1;
