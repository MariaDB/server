connection con1;
truncate table test;
insert into test (id, value) values (1, 10), (2, 20);
begin;
connection con2;
begin;
connection con3;
begin;
