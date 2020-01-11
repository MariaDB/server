insert into tt (id) values (1),(2),(3) on duplicate key update x = 0;
insert into tt (id) values (1) on duplicate key update y = 0, z = 42;
insert into tt (id) values (1) on duplicate key update y = y + 1, z = z + 50;
insert into tt (id) values (1) on duplicate key update y = y - 1;
insert into tt (id) values (1) on duplicate key update z = z - 50;

create table ti like tt;
alter table ti engine=innodb;
insert into ti (id) values (1),(2),(3) on duplicate key update x = 0;
insert into ti (id) values (1) on duplicate key update y = 0, z = 42;
insert into ti (id) values (1) on duplicate key update y = y + 1, z = z + 50;
insert into ti (id) values (1) on duplicate key update y = y - 1;
insert into ti (id) values (1) on duplicate key update z = z - 50;

let $diff_tables = test.tt, test.ti;
source include/diff_tables.inc;

drop table tt, ti;

