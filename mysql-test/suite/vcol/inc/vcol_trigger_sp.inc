################################################################################
# inc/vcol_trigger_sp.inc                                                      #
#                                                                              #
# Purpose:                                                                     #
#  Testing triggers, stored procedures and functions                           #
#  defined on tables with virtual columns.                                     #
#                                                                              #
#                                                                              #
#                                                                              #
#------------------------------------------------------------------------------#
# Original Author: Andrey Zhakov                                               #
# Original Date: 2008-09-04                                                    #
# Change Author: Oleksandr Byelkin (Monty program Ab)
# Date: 2009-03-24 
# Change: Syntax changed
################################################################################

create table t1 (a int, 
                 b int as (a/10),
		 c int as (a/10) persistent);

create table t2 (a timestamp);

delimiter |;

create trigger trg1 before insert on t1 for each row
begin
  if (new.b < 10) then
    set new.a:= 100;
    set new.b:= 9;
    set new.c:= 9;
  end if;

  if (new.c > 50) then
    set new.a:= 500; 
  end if;
end|

create trigger trg2 after insert on t1 for each row
begin
  if (new.b >= 60) then
    insert into t2 values (now()); 
  end if;
end|

create function f1()
returns int
begin
  declare sum1 int default '0';
  declare cur1 cursor for select sum(b) from t1;
  open cur1;
  fetch cur1 into sum1;
  close cur1;
  return sum1;
end|

delimiter ;|

set sql_warnings = 1;

insert into t1 (a) values (200);
select * from t1;
select * from t2;

insert into t1 (a) values (10);
select * from t1;
select * from t2;

insert into t1 (a) values (600);
select * from t1;
--replace_column 1 <timestamp>
select * from t2;

select f1();

set sql_warnings = 0;

drop trigger trg1;
drop trigger trg2;
drop table t2;

delimiter |;

create procedure p1()
begin
  declare i int default '0';
  create table t2 like t1;
  insert into t2 (a) values (100), (200);
  begin
    declare cur1 cursor for select sum(c) from t2;
    open cur1;
    fetch cur1 into i;
    close cur1;
    if (i=30) then
      insert into t1 values (300,default,default);
    end if;
  end;
end|

delimiter ;|

delete from t1;

call p1();

select * from t2;
select * from t1;

drop table t1,t2;
drop procedure p1;

--echo #
--echo # MDEV-3845 values of virtual columns are not computed for triggers
--echo #

CREATE TABLE t1 (
  a INTEGER UNSIGNED NULL DEFAULT NULL,
  b INTEGER UNSIGNED GENERATED ALWAYS AS (a) VIRTUAL
);

CREATE TABLE t2 (c INTEGER UNSIGNED NOT NULL);

DELIMITER |;

CREATE TRIGGER t1_ins_aft
  AFTER INSERT
  ON t1
  FOR EACH ROW
BEGIN
  INSERT INTO t2 (c) VALUES (NEW.b);
END |

CREATE TRIGGER t1_del_bef
  BEFORE DELETE
  ON t1
  FOR EACH ROW
BEGIN
  INSERT INTO t2 (c) VALUES (OLD.b);
END |

DELIMITER ;|

INSERT INTO t1 (a) VALUES (1), (2), (3);
SELECT * FROM t2;
DELETE FROM t1;
SELECT * FROM t2;

DROP TRIGGER t1_ins_aft;
DROP TRIGGER t1_del_bef;
DROP TABLE t1,t2;

#
# MDEV-11706 Assertion `is_stat_field || !table || (!table->write_set || bitmap_is_set(table->write_set, field_index) || (table->vcol_set && bitmap_is_set(table->vcol_set, field_index)))' failed in Field_time::store_TIME_with_warning
#
create table t1 (i int, t time not null, vt time(4) as (t) virtual);
create trigger trg before update on t1 for each row set @a = 1;
insert ignore into t1 (i) values (1);
drop table t1;

--echo #
--echo # Examine the number of times triggers are recalculated for updates
--echo #
SET sql_mode = 'NO_ENGINE_SUBSTITUTION';
CREATE TABLE t1 (
  a INTEGER UNSIGNED NULL DEFAULT NULL,
  b CHAR(10) NULL DEFAULT NULL,
  c blob NULL DEFAULT NULL,
  blob_a blob GENERATED ALWAYS AS (last_value(@a:=@a+1,a)) VIRTUAL,
  blob_b blob GENERATED ALWAYS AS (last_value(@b:=@b+1,b)) VIRTUAL,
  blob_c blob GENERATED ALWAYS AS (last_value(@c:=@c+1,c)) VIRTUAL
);

DELIMITER |;
CREATE TRIGGER t1_ins
  BEFORE INSERT
  ON t1
  FOR EACH ROW
BEGIN
 IF NEW.b IS NULL THEN
     SET NEW.b="generated before insert";
 END IF;
END |

CREATE TRIGGER t1_update
  BEFORE UPDATE
  ON t1
  FOR EACH ROW
BEGIN
  IF NEW.b IS NULL or NEW.c IS NULL THEN
     SET NEW.b="generated before update";
     SET NEW.c="generated before update";
  END IF;
END |

DELIMITER ;|

--echo # Inserts
set @a=0,@b=0,@c=0;

insert into t1 (a) values(1);
insert into t1 (a,b) values(2, "*2*");
insert into t1 (a,b,c) values(3, "*3*", "**3**");
insert into t1 (a,c) values(4, "**4**");
select * from t1;
select @a,@b,@c;
select * from t1;
select @a,@b,@c;
select a,b,c from t1;
select @a,@b,@c;
select a,b,c,blob_a from t1;
select @a,@b,@c;

--echo # updates
set @a=0,@b=0,@c=0;

update t1 set a=a+100 where a=1;
update t1 set a=a+100, b="*102*" where a=2;
update t1 set a=a+100, b=NULL where a=3;
update t1 set a=a+100, b="invisible", c=NULL where a=4;
select @a,@b,@c;
select * from t1;

drop trigger t1_ins;
drop trigger t1_update;
drop table t1;

SET sql_mode = DEFAULT;

--echo #
--echo # Same test, but with virtual keys
--echo #
SET sql_mode = 'NO_ENGINE_SUBSTITUTION';
CREATE TABLE t1 (
  a INTEGER UNSIGNED NULL DEFAULT NULL,
  b CHAR(10) NULL DEFAULT NULL,
  c blob NULL DEFAULT NULL,
  blob_a blob GENERATED ALWAYS AS (a) VIRTUAL,
  blob_b blob GENERATED ALWAYS AS (RTRIM(b)) VIRTUAL,
  blob_c blob GENERATED ALWAYS AS (c) VIRTUAL,
  key (a),
  key (blob_a(10)),
  key (blob_b(10)),
  key (blob_c(10))
);

DELIMITER |;
CREATE TRIGGER t1_ins
  BEFORE INSERT
  ON t1
  FOR EACH ROW
BEGIN
 IF NEW.b IS NULL THEN
     SET NEW.b="generated before insert";
 END IF;
END |

CREATE TRIGGER t1_update
  BEFORE UPDATE
  ON t1
  FOR EACH ROW
BEGIN
  IF NEW.b IS NULL or NEW.c IS NULL THEN
     SET NEW.b="generated before update";
     SET NEW.c="generated before update";
  END IF;
END |

DELIMITER ;|

--echo # Inserts
insert into t1 (a) values(1);
insert into t1 (a,b) values(2, "*2*");
insert into t1 (a,b,c) values(3, "*3*", "**3**");
insert into t1 (a,c) values(4, "**4**");
select * from t1;
select @a,@b,@c;
select * from t1;
select @a,@b,@c;
select a,b,c from t1;
select @a,@b,@c;
select a,b,c,blob_a from t1;
select @a,@b,@c;

--echo # updates
update t1 set a=a+100 where a=1;
update t1 set a=a+100, b="*102*" where a=2;
update t1 set a=a+100, b=NULL where a=3;
update t1 set a=a+100, b="invisible", c=NULL where a=4;
select * from t1;

drop trigger t1_ins;
drop trigger t1_update;
drop table t1;
SET sql_mode = DEFAULT;
