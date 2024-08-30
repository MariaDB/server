--echo #
--echo # Delete with value from subquery on the same table
--echo #

analyze table t1 persistent for all;

let $c = c1=(select a.c3 from t1 a where a.c3 = t1.c3);
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
--echo # Delete with EXISTS subquery over the updated table
--echo # in WHERE + possibly sargable condition
--echo #

analyze table t1 persistent for all;

let $c = c1 = 1 and exists (select 'X' from t1 a where a.c1 = t1.c2);
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
eval explain select * from t1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;

--echo #
--echo # Delete with IN predicand over the updated table in WHERE
--echo #
let $c = c3 in (select distinct a.c1 from t1 a where t1.c2=a.c2);
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
--echo # Delete with a limit - can be deleted
--echo #
let $c = c1 in (select a.c2 from t1 a where a.c2 = t1.c3) limit 1;
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
--echo # Delete with a limit and an order by
--echo #

let $c = c1 in (select a.c2 from t1 a where a.c3 = t1.c3)
                order by c3 desc limit 1;
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
--echo # Delete: 2 execution of PS
--echo #

prepare create_tmp_stmt from
    "create table tmp as select * from t1
      where c2=(select a.c3 from t1 a where a.c3 = ?)";
prepare delete_t1_stmt from
    "delete from t1 where c2=(select a.c3 from t1 a where a.c3 = ?)";
set @a:=5;
execute create_tmp_stmt using @a;
execute delete_t1_stmt using @a;
execute delete_t1_stmt using @a;
--sorted_result
select * from t1;

prepare insert_tmp_stmt from
    "insert into tmp(c1,c2,c3) select * from t1
      where c2=(select a.c3 from t1 a where a.c3 = ?)";
set @a:=2;
execute insert_tmp_stmt using @a;
execute delete_t1_stmt using @a;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
--sorted_result
select * from t1;

drop table tmp;

--echo #
--echo # Delete in stored procedure
--echo #

delimiter //;
create procedure sp()
begin
  delete from t1 where c1 in (select a.c2 from t1 a where a.c3 = t1.c3)
    order by c3 desc limit 1;
end
//
delimiter ;//

create table tmp as select * from t1
  where c1 in (select a.c2 from t1 a where a.c3 = t1.c3)
    order by c3 desc limit 1;
CALL sp;
insert into tmp(c1,c2,c3) select * from t1
  where c1 in (select a.c2 from t1 a where a.c3 = t1.c3)
    order by c3 desc limit 1;
CALL sp;
insert into tmp(c1,c2,c3) select * from t1
  where c1 in (select a.c2 from t1 a where a.c3 = t1.c3)
    order by c3 desc limit 1;
CALL sp;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop procedure sp;
drop table tmp;

--echo #
--echo # Delete in stored function
--echo #
delimiter //;
create function f1(IN a INT) returns int
begin
  delete from t1 where c3 < a  order by c3 limit 1;
  return  1;
end;//
delimiter ;//

set @a:=7;
create table tmp as select * from t1 where c3 < @a
  order by c3 limit 1;
--disable_ps2_protocol # because SELECT with side effects
select f1(@a);
insert into tmp(c1,c2,c3) select * from t1 where c3 < @a
  order by c3 limit 1;
select f1(@a);
--enable_ps2_protocol
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop function f1;
drop table tmp;

--echo #
--echo # Delete in trigger
--echo #

create table t2 (c1 integer);
insert into t2(c1) values (1), (2), (3), (4), (5), (6), (7), (8);

CREATE TABLE cnt(del integer);
INSERT INTO cnt VALUES(0);

CREATE TRIGGER tr1 AFTER DELETE ON t1 FOR EACH ROW
  UPDATE cnt SET del=del+1;
CREATE TRIGGER tr2 AFTER DELETE ON t1 FOR EACH ROW
  DELETE FROM t2 WHERE c1> (SELECT count(*)-1 FROM t2);

CREATE TABLE tmp as SELECT * FROM t1 WHERE c2>=3;
--enable_info ONCE
DELETE FROM t1 WHERE c2>=3;

--sorted_result
select * from t1;
--sorted_result
SELECT * FROM t2;
SELECT * FROM cnt;

insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;

DROP TRIGGER tr1;
DROP TRIGGER tr2;
drop table t2, cnt, tmp;

--echo #
--echo Delete with a reference to view in subquery
--echo #
let $c = t1.c2 in ( select max(a.c2) from v1 a
                      where a.c1 = t1.c1);
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
eval explain select * from v1 where $c;
eval explain $q;
--enable_info ONCE
eval $q;
--sorted_result
select * from t1;
insert into t1(c1,c2,c3) select c1,c2,c3 from tmp;
drop table tmp;

