--echo #
--echo # Start of ctype_thai.inc
--echo #

#
# Test Unicode Thai collations based on libthai testcases
#

CREATE TABLE t1 (a VARCHAR(30) CHARACTER SET utf8 COLLATE utf8_bin);

INSERT INTO t1 VALUES ('-กระแย่ง');
INSERT INTO t1 VALUES ('กก');
INSERT INTO t1 VALUES ('กราบ');
INSERT INTO t1 VALUES ('ข่าง');
INSERT INTO t1 VALUES ('ข้าง');
INSERT INTO t1 VALUES ('ข้างกระดาน');
INSERT INTO t1 VALUES ('ข้างขึ้น');
INSERT INTO t1 VALUES ('ข้างควาย');
INSERT INTO t1 VALUES ('ข้างเงิน');
INSERT INTO t1 VALUES ('ข้างแรม');
INSERT INTO t1 VALUES ('ข้างออก');
INSERT INTO t1 VALUES ('ข้างๆ');
INSERT INTO t1 VALUES ('ข้างๆ คูๆ');
INSERT INTO t1 VALUES ('ขาง');
INSERT INTO t1 VALUES ('แข็ง');
INSERT INTO t1 VALUES ('แข่ง');
INSERT INTO t1 VALUES ('แข่งขัน');
INSERT INTO t1 VALUES ('แข้ง');
INSERT INTO t1 VALUES ('แข้งขวา');
INSERT INTO t1 VALUES ('แข็งขัน');
INSERT INTO t1 VALUES ('ทูลเกล้า');
INSERT INTO t1 VALUES ('ทูลเกล้าทูลกระหม่อม');
INSERT INTO t1 VALUES ('ทูลเกล้าฯ');
INSERT INTO t1 VALUES ('บุญญา');
INSERT INTO t1 VALUES ('บุญ-หลง');
INSERT INTO t1 VALUES ('บุญหลง');
INSERT INTO t1 VALUES ('ป่า');
INSERT INTO t1 VALUES ('ป่าน');
INSERT INTO t1 VALUES ('ป้า');
INSERT INTO t1 VALUES ('ป้าน');
INSERT INTO t1 VALUES ('ป๊า');
INSERT INTO t1 VALUES ('ป๊าน');
INSERT INTO t1 VALUES ('ป๋า');
INSERT INTO t1 VALUES ('ป๋าน');
INSERT INTO t1 VALUES ('ปา');
INSERT INTO t1 VALUES ('ปาน');
INSERT INTO t1 VALUES ('แป้ง');
INSERT INTO t1 VALUES ('พณิชย์');
INSERT INTO t1 VALUES ('ม้า');
INSERT INTO t1 VALUES ('ฯพณฯ');
INSERT INTO t1 VALUES ('A');
INSERT INTO t1 VALUES ('a');
INSERT INTO t1 VALUES ('a\'');
INSERT INTO t1 VALUES ('A-');
INSERT INTO t1 VALUES ('a-');
INSERT INTO t1 VALUES ('-a');
INSERT INTO t1 VALUES ('A.');
INSERT INTO t1 VALUES ('a.');
INSERT INTO t1 VALUES ('A-1');
INSERT INTO t1 VALUES ('aa');
INSERT INTO t1 VALUES ('AA');
INSERT INTO t1 VALUES ('A.A.');
INSERT INTO t1 VALUES ('a.a.');
INSERT INTO t1 VALUES ('AAA');
INSERT INTO t1 VALUES ('A.A.A.');
INSERT INTO t1 VALUES ('AAAA');
INSERT INTO t1 VALUES ('A.A.A.L.');
INSERT INTO t1 VALUES ('A.A.A.S.');
INSERT INTO t1 VALUES ('Aachen');
INSERT INTO t1 VALUES ('A.A.E.');
INSERT INTO t1 VALUES ('A.Ae.E.');
INSERT INTO t1 VALUES ('A.A.E.E.');
INSERT INTO t1 VALUES ('AAES');
INSERT INTO t1 VALUES ('AAF');
INSERT INTO t1 VALUES ('A.Agr');
INSERT INTO t1 VALUES ('aah');
INSERT INTO t1 VALUES ('@@@@@');
INSERT INTO t1 VALUES ('0000');
INSERT INTO t1 VALUES ('9999');
INSERT INTO t1 VALUES ('Aalborg');
INSERT INTO t1 VALUES ('aide');
INSERT INTO t1 VALUES ('air');
INSERT INTO t1 VALUES ('@@@air');
INSERT INTO t1 VALUES ('air@@@');
INSERT INTO t1 VALUES ('C.A.F');
INSERT INTO t1 VALUES ('Canon');
INSERT INTO t1 VALUES ('coop');
INSERT INTO t1 VALUES ('co-op');
INSERT INTO t1 VALUES ('COOP');
INSERT INTO t1 VALUES ('CO-OP');
INSERT INTO t1 VALUES ('Copenhegen');
INSERT INTO t1 VALUES ('McArthur');
INSERT INTO t1 VALUES ('Mc Arthur');
INSERT INTO t1 VALUES ('Mc Mahon');
INSERT INTO t1 VALUES ('vice-president');
INSERT INTO t1 VALUES ('vice versa');
INSERT INTO t1 VALUES ('vice-versa');
INSERT INTO t1 VALUES ('10 ลิตร');
INSERT INTO t1 VALUES ('๑๐ ลิตร');
INSERT INTO t1 VALUES ('10 litre');
INSERT INTO t1 VALUES ('10 litre (10 ลิตร)');
INSERT INTO t1 VALUES ('10 ลิตร (10 litre)');
INSERT INTO t1 VALUES ('10 litre (๑๐ ลิตร)');
INSERT INTO t1 VALUES ('๑๐ ลิตร (10 litre)');
INSERT INTO t1 VALUES ('๑๐ ลิตร [10 litre]');
INSERT INTO t1 VALUES ('๑๐ ลิตร {10 litre}');

# Sort the table according to binary 'a'
# To avoid problems with unpredictable order or 'AA' and 'aa'
ALTER TABLE t1 ORDER BY a;

SET @backup_character_set_connection=@@character_set_connection;
SET @backup_collation_connection=@@collation_connection;
SET NAMES utf8;
SET @stmt= CONCAT('ALTER TABLE t1 MODIFY a VARCHAR(30)',
                  ' CHARACTER SET ', @backup_character_set_connection,
                  ' COLLATE ', @backup_collation_connection,
                  ' , ADD id INT NOT NULL AUTO_INCREMENT PRIMARY KEY'
                  ' , ADD KEY a_id (a, id)');
PREPARE stmt FROM @stmt;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
SET collation_connection=@backup_collation_connection;

SHOW CREATE TABLE t1;

--echo #
--echo # Ascending sort, using filesort
--echo #
EXPLAIN SELECT a FROM t1 ORDER BY a, BINARY a;
SELECT a FROM t1 ORDER BY a, BINARY a;

--echo #
--echo # Descending sort, using filesort
--echo #
EXPLAIN SELECT a FROM t1 ORDER BY a DESC, BINARY a DESC;
SELECT a FROM t1 ORDER BY a DESC, BINARY a DESC;


--echo #
--echo # Ascending sort, using index
--echo #
EXPLAIN SELECT a FROM t1 ORDER BY a, id;
SELECT a FROM t1 ORDER BY a, id;

--echo #
--echo # Descending sort, using index
--echo #
EXPLAIN SELECT a FROM t1 ORDER BY a DESC, id DESC;
SELECT a FROM t1 ORDER BY a DESC, id DESC;

DROP TABLE t1;

--echo #
--echo # End of ctype_thai.inc
--echo #
