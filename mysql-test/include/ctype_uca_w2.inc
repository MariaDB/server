--echo #
--echo # Start of ctype_uca_w2.inc
--echo #

SELECT @@collation_connection;

SELECT ID, SORTLEN, COLLATION_NAME, CHARACTER_SET_NAME
FROM INFORMATION_SCHEMA.COLLATIONS
WHERE COLLATION_NAME LIKE @@collation_connection;

--echo #
--echo # Testing strnxfrm
--echo #

CREATE TABLE t1 AS SELECT SPACE(3) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (SPACE(0)),(SPACE(1)),(SPACE(2));
SELECT HEX(a), HEX(WEIGHT_STRING(a AS CHAR(8))) FROM t1;
SELECT HEX(a), HEX(WEIGHT_STRING(a AS CHAR(4))) FROM t1;
SELECT HEX(a), HEX(WEIGHT_STRING(a AS CHAR(4) LEVEL 1)) FROM t1;
SELECT HEX(a), HEX(WEIGHT_STRING(a AS CHAR(4) LEVEL 2)) FROM t1;
SELECT HEX(a), HEX(WEIGHT_STRING(a AS CHAR(4) LEVEL 3)) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT SPACE(10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (_ucs2 0x3400);
INSERT INTO t1 VALUES (_ucs2 0xF001);
SELECT HEX(CONVERT(a USING ucs2)) AS ucs2, HEX(a), HEX(WEIGHT_STRING(a)) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT SPACE(10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('A'),('À'),('Á'),('Â'),('Ã'),('Ä'),('Å');
SELECT a, HEX(WEIGHT_STRING(a LEVEL 2)) FROM t1 ORDER BY a;
SELECT a, HEX(WEIGHT_STRING(a LEVEL 2)) FROM t1 ORDER BY a DESC;

SET @backup_character_set_connection=@@character_set_connection;
SET @backup_collation_connection=@@collation_connection;

SET NAMES utf8;
SET @stmt= CONCAT('ALTER TABLE t1 MODIFY a CHAR(10)' COLLATE utf8_bin,
                  ' CHARACTER SET ',  @backup_character_set_connection,
                  ' COLLATE ',  @backup_collation_connection);
PREPARE stmt FROM @stmt;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
SET collation_connection=@backup_collation_connection;

SHOW CREATE TABLE t1;
SELECT a, HEX(WEIGHT_STRING(a LEVEL 2)) FROM t1 ORDER BY a;
SELECT a, HEX(WEIGHT_STRING(a LEVEL 2)) FROM t1 ORDER BY a DESC;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT SPACE(10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('cota'),('cote'),('cotz');
INSERT INTO t1 VALUES ('coté'),('côte'),('côté'),('cotë'),('côtë');
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t1 ORDER BY a DESC;
DROP TABLE t1;

--echo #
--echo # End of ctype_uca_w2.inc
--echo #
