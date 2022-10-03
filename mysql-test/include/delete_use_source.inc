create table t1 (c1 integer, c2 integer, c3 integer);

insert into t1(c1,c2,c3) values (1,1,1);
insert into t1(c1,c2,c3) values (1,2,2);
insert into t1(c1,c2,c3) values (1,3,3);
insert into t1(c1,c2,c3) values (2,1,4);
insert into t1(c1,c2,c3) values (2,2,5);
insert into t1(c1,c2,c3) values (2,3,6);
insert into t1(c1,c2,c3) values (2,4,7);
insert into t1(c1,c2,c3) values (2,5,8);

create view v1 as select * from t1 where c2=2;
--let $with_view=1

--echo Test without any index
--source include/delete_use_source_cases.inc

--echo Test with an index
create index t1_c2 on t1 (c2,c1);
--source include/delete_use_source_cases.inc

--echo Test with a primary key
drop index t1_c2 on t1;
alter table t1 add primary key (c3);
--source include/delete_use_source_cases.inc

drop view v1;
drop table t1;

create temporary table t1 (c1 integer, c2 integer, c3 integer);
insert into t1(c1,c2,c3) values (1,1,1);
insert into t1(c1,c2,c3) values (1,2,2);
insert into t1(c1,c2,c3) values (1,3,3);
insert into t1(c1,c2,c3) values (2,1,4);
insert into t1(c1,c2,c3) values (2,2,5);
insert into t1(c1,c2,c3) values (2,3,6);
insert into t1(c1,c2,c3) values (2,4,7);
insert into t1(c1,c2,c3) values (2,5,8);

--let $with_view=0

--echo Test without any index
--source include/delete_use_source_cases.inc

--echo Test with an index
create index t1_c2 on t1 (c2,c1);
--source include/delete_use_source_cases.inc

--echo Test with a primary key
drop index t1_c2 on t1;
alter table t1 add primary key (c3);
--source include/delete_use_source_cases.inc

drop table t1;


--echo #
--echo # Test on dynamic columns (blob)
--echo #
create table assets (
  item_name varchar(32) primary key, -- A common attribute for all items
  dynamic_cols  blob  -- Dynamic columns will be stored here
);
INSERT INTO assets VALUES ('MariaDB T-shirt', COLUMN_CREATE('color', 'blue', 'size', 'XL'));
INSERT INTO assets VALUES ('Thinkpad Laptop', COLUMN_CREATE('color', 'black', 'price', 500));
INSERT INTO assets VALUES ('Fridge', COLUMN_CREATE('color', 'white', 'warranty', '5 years'));
INSERT INTO assets VALUES ('Microwave', COLUMN_CREATE('warranty', '3 years'));
SELECT item_name, COLUMN_GET(dynamic_cols, 'color' as char) AS color FROM assets ORDER BY item_name;
UPDATE assets SET dynamic_cols=COLUMN_DELETE(dynamic_cols, 'color') WHERE item_name='Fridge';
SELECT item_name, COLUMN_GET(dynamic_cols, 'color' as char) AS color FROM assets ORDER BY item_name;
DELETE FROM assets
 WHERE item_name in (select b.item_name
                       from assets b
                      where COLUMN_GET(b.dynamic_cols, 'color' as char) ='black');
SELECT item_name, COLUMN_GET(dynamic_cols, 'color' as char) AS color FROM assets ORDER BY item_name;
DELETE FROM assets WHERE item_name='Microwave';
SELECT item_name, COLUMN_GET(dynamic_cols, 'color' as char) AS color FROM assets ORDER BY item_name;
drop table assets ;


--echo #
--echo # Test on fulltext columns
--echo #
CREATE TABLE ft2(copy TEXT,FULLTEXT(copy));
INSERT INTO ft2(copy) VALUES
 ('MySQL vs MariaDB database'),
 ('Oracle vs MariaDB database'),
 ('PostgreSQL vs MariaDB database'),
 ('MariaDB overview'),
 ('Foreign keys'),
 ('Primary keys'),
 ('Indexes'),
 ('Transactions'),
 ('Triggers');

SELECT * FROM ft2 WHERE MATCH(copy) AGAINST('database');
DELETE FROM ft2 WHERE  MATCH(copy) AGAINST('database');
SELECT * FROM ft2 WHERE MATCH(copy) AGAINST('database');
drop table ft2;

