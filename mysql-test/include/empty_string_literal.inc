SET SESSION character_set_connection=latin2;
SET SESSION character_set_client=cp1250;

--echo #
--echo # Test litteral
--echo #
SET sql_mode=@mode;
select @@sql_mode;
SELECT '',CHARSET(''), null, CHARSET(null), CAST(null as char(10)), CHARSET(CAST(null as char(10))), 'x', CHARSET('x');
SELECT CHARSET(NULLIF('','')),NULLIF('','');
SET sql_mode=default;
SELECT '',CHARSET(''), null, CHARSET(null), CAST(null as char(10)), CHARSET(CAST(null as char(10))), 'x', CHARSET('x');
SELECT CHARSET(NULLIF('','')),NULLIF('','');

--echo #
--echo # Test NCHAR litteral
--echo #
SET sql_mode=@mode;
SELECT N'',CHARSET(N''), N'x', CHARSET(N'x');
SELECT CHARSET(NULLIF(N'',N'')),NULLIF(N'',N'');
SET sql_mode=default;
SELECT N'',CHARSET(N''), N'x', CHARSET(N'x');
SELECT CHARSET(NULLIF(N'',N'')),NULLIF(N'',N'');

--echo #
--echo # Test CHARSET prefix litteral
--echo #
SET sql_mode=@mode;
SELECT _cp1250 '',CHARSET(_cp1250 ''), _cp1250 'x', CHARSET(_cp1250 'x');
SELECT CHARSET(NULLIF(_cp1250 '',_cp1250 '')),NULLIF(_cp1250 '',_cp1250 '');
SET sql_mode=default;
SELECT _cp1250 '',CHARSET(_cp1250 ''), _cp1250 'x', CHARSET(_cp1250 'x');
SELECT CHARSET(NULLIF(_cp1250 '',_cp1250 '')),NULLIF(_cp1250 '',_cp1250 '');


SET sql_mode=@mode;

--echo #
--echo # Test litteral concat
--echo #
SELECT 'a' 'b';
SELECT 'a' '';
SELECT '' 'b';
SELECT '' '';
SELECT '' 'b' 'c';
SELECT '' '' 'c';
SELECT 'a' '' 'c';
SELECT 'a' '' '';
SELECT '' '' '';

SELECT '' '' '',CHARSET('' '' '');
SELECT _latin1'' '' '',CHARSET(_latin1'' '' '');
SELECT N'' '' '',CHARSET(N'' '' '');

--echo #
--echo # UNION - implicit group by
--echo #
SELECT 1, null
UNION
SELECT 1 , ''
ORDER BY 1;

SELECT 1, null
UNION
SELECT 1 , N''
ORDER BY 1;

SELECT 1, null
UNION
SELECT 1 , _cp1250 ''
ORDER BY 1;

SELECT NULLIF(_cp1250 '',_cp1250 '')
UNION
SELECT NULLIF(N'',N'');

--error ER_CANT_AGGREGATE_2COLLATIONS
SELECT 1 , _latin2 ''
UNION
SELECT 1 , _cp1250 '';

SELECT 1, null
UNION
SELECT 1 , ''
UNION
SELECT 1 , N'';

CREATE TABLE t1 (c1 INT,c2 VARCHAR(10));
INSERT INTO t1 VALUES (1,'one');
INSERT INTO t1 VALUES (1,'');
INSERT INTO t1 VALUES (1,null);

--echo #
--echo # Test in a view
--echo #
CREATE VIEW v1
    AS SELECT c1, c2
         FROM t1
       UNION
       SELECT c1 , ''
         FROM t1
       ORDER BY 1,2;
SELECT * FROM v1;

SHOW CREATE VIEW v1;

DROP VIEW v1;
DROP TABLE t1;

EXPLAIN EXTENDED SELECT '';
EXPLAIN EXTENDED SELECT _latin1'';
EXPLAIN EXTENDED SELECT N'';
EXPLAIN EXTENDED SELECT '' '';
