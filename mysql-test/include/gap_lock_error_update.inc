eval set session autocommit=$autocommit;
let $is_gaplock_target = `SELECT @@autocommit = 0 && '$expect_gap_lock_errors' = 1`;

if ($is_gaplock_target)
{
## single-table insert,update,delete
insert into gap1 (id1, id2, id3) values (-1,-1,-1);
insert into gap1 (id1, id2, id3) values (-1,-1,-1)
  on duplicate key update value=100;
--error ER_UNKNOWN_ERROR
update gap1 set value=100 where id1=1;
update gap1 set value=100 where id1=1 and id2=1 and id3=1;
--error ER_UNKNOWN_ERROR
delete from gap1 where id1=2;
delete from gap1 where id1=-1 and id2=-1 and id3=-1;
commit;

## multi-table statements (preventing all gap locks with autocommit)
# insert into select
--error ER_UNKNOWN_ERROR
insert into gap2 select * from gap1;
--error ER_UNKNOWN_ERROR
insert into gap2 select * from gap1 where id1=1;
insert into gap2 select * from gap1 where id1=1 and id2=1 and id3=1;

# create table select
create table t4 select * from gap1 where id1=1 and id2=1 and id3=1;
drop table t4;
--error ER_UNKNOWN_ERROR
create table t4 select * from gap1;
--error ER_UNKNOWN_ERROR
create table t4 select * from gap1 where id1=1;

# update join
update gap1 join gap2 on gap1.id1 and gap1.id2=gap2.id2 set gap1.value=100 where gap2.id1=3
  and gap2.id2=3 and gap2.id3=3;
--error ER_UNKNOWN_ERROR
update gap1 join gap2 on gap1.id1 and gap1.id2=gap2.id2 set gap1.value=100 where gap2.id1=3;
--error ER_UNKNOWN_ERROR
update gap1 join gap2 on gap1.id1 and gap1.id2=gap2.id2 join gap3 on gap1.id1=gap3.id
  set gap1.value=100 where gap2.id1=3;
--error ER_UNKNOWN_ERROR
update gap1 set gap1.value= (select count(*) from gap2);

# delete join
delete gap1 from gap1 join gap2 on gap1.id1 and gap1.id2=gap2.id2 where gap2.id1=3
  and gap2.id2=3 and gap2.id3=3;
--error ER_UNKNOWN_ERROR
delete gap1 from gap1 join gap2 on gap1.id1 and gap1.id2=gap2.id2 where gap2.id1=3;

# select join / self join
--error ER_UNKNOWN_ERROR
select * from gap1, gap2 limit 1 for update;
--error ER_UNKNOWN_ERROR
select * from gap1 a, gap1 b limit 1 for update;

# unique secondary key
create table u1(
 c1 int,
 c2 int,
 c3 int,
 c4 int,
 primary key (c1, c2, c3),
 unique key (c3, c1)
);
begin;
insert into u1 values (1,1,1,1);
commit;
begin;
insert into u1 values (1,2,1,1) on duplicate key update c4=10;
commit;
begin;
select * from u1 where c3=1 and c1 = 1 for update;
--error ER_UNKNOWN_ERROR
select * from u1 where c3=1 for update;
commit;
drop table u1;
}

if (!$is_gaplock_target)
{
# autocommit doesn't prevent single table operations
insert into gap1 (id1, id2, id3) values (-1,-1,-1);
insert into gap1 (id1, id2, id3) values (-1,-1,-1)
  on duplicate key update value=100;
update gap1 set value=100 where id1=1;
update gap1 set value=100 where id1=1 and id2=1 and id3=1;
delete from gap1 where id1=2;
delete from gap1 where id1=-1 and id2=-1 and id3=-1;
commit;
}
