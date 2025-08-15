--echo #
--echo # Range optimizer
--echo #

CREATE TABLE t1 (a INET6, INDEX(a));
SHOW CREATE TABLE t1;

DELIMITER $$;
FOR i IN 0..255
DO
  INSERT INTO t1 VALUES (CONCAT('::', HEX(i)));
END FOR
$$
DELIMITER ;$$
SELECT * FROM t1 WHERE a='::ff';
EXPLAIN SELECT * FROM t1 WHERE a='::ff';
SELECT * FROM t1 WHERE a='garbage';
EXPLAIN SELECT * FROM t1 WHERE a='garbage';

SELECT * FROM t1 WHERE a>='::fe';
EXPLAIN SELECT * FROM t1 WHERE a>='::fe';
SELECT * FROM t1 WHERE a>='garbage';
EXPLAIN SELECT * FROM t1 WHERE a>='garbage';

SELECT * FROM t1 WHERE a IN ('::80','::a0','::f0');
EXPLAIN SELECT * FROM t1 WHERE a IN ('::80','::a0','::f0');
SELECT * FROM t1 WHERE a IN ('::80','::a0','garbage');
EXPLAIN SELECT * FROM t1 WHERE a IN ('::80','::a0','garbage');

SELECT * FROM t1 WHERE a BETWEEN '::80' AND '::81';
EXPLAIN SELECT * FROM t1 WHERE a BETWEEN '::80' AND '::81';
SELECT * FROM t1 WHERE a BETWEEN '::80' AND 'garbage';
EXPLAIN SELECT * FROM t1 WHERE a BETWEEN '::80' AND 'garbage';

SELECT * FROM t1 WHERE a=CAST('::ff' AS INET6);
EXPLAIN EXTENDED SELECT * FROM t1 WHERE a=CAST('::ff' AS INET6);

DROP TABLE t1;

--echo #
--echo # MDEV-34922: Assertion `value.length() == FbtImpl::binary_length()' failed in
--echo # Type_handler_fbt<FbtImpl, TypeCollectionImpl>::Field_fbt::store_native,
--echo # Assertion `item->null_value' failed in Type_handler::Item_send_str
--echo #

CREATE TABLE t1 (a datetime);
INSERT INTO t1 VALUES (NULL);
SELECT * FROM (SELECT cast('::' AS INET6),min(1) FROM t1 WHERE if(uuid_short(), a,1)) dt;
DROP TABLE t1;

--echo # End of 10.5 tests
