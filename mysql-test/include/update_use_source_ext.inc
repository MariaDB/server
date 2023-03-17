--echo #
--echo # Test on dynamic columns (blob)
--echo #

create table assets (
  item_name varchar(32) primary key, -- A common attribute for all items
  dynamic_cols  blob  -- Dynamic columns will be stored here
);
INSERT INTO assets VALUES ('MariaDB T-shirt',
    COLUMN_CREATE('color', 'blue', 'size', 'XL'));
INSERT INTO assets VALUES ('Thinkpad Laptop',
    COLUMN_CREATE('color', 'black', 'price', 500));
SELECT item_name, COLUMN_GET(dynamic_cols, 'color' as char) AS color
    FROM assets;
UPDATE assets
  SET dynamic_cols=COLUMN_ADD(dynamic_cols, 'warranty', '3 years')
    WHERE item_name='Thinkpad Laptop';
SELECT item_name,
  COLUMN_GET(dynamic_cols, 'warranty' as char) AS color
    FROM assets;
UPDATE assets
  SET dynamic_cols=COLUMN_ADD(dynamic_cols, 'warranty', '4 years')
    WHERE item_name in
      (select b.item_name from assets b
        where COLUMN_GET(b.dynamic_cols, 'color' as char) ='black');
SELECT item_name,
  COLUMN_GET(dynamic_cols, 'warranty' as char) AS color
    FROM assets;

UPDATE assets SET dynamic_cols=COLUMN_ADD(dynamic_cols, 'warranty',
    (select COLUMN_GET(b.dynamic_cols, 'color' as char)
      from assets b
      where assets.item_name = item_name));
SELECT item_name,
  COLUMN_GET(dynamic_cols, 'warranty' as char) AS color
    FROM assets;
drop table assets;

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

update ft2 set copy = (select max(concat('mykeyword ',substr(b.copy,1,5)))
  from ft2 b WHERE MATCH(b.copy) AGAINST('database'))
  where MATCH(copy) AGAINST('keys');
SELECT * FROM ft2 WHERE MATCH(copy) AGAINST('mykeyword');
drop table ft2;
