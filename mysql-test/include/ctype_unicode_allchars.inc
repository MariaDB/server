#
# Make a table with all Unicode characters
# in the range U+0000 .. U+10FFFF
#

CREATE TABLE allchars AS SELECT 1 AS code, ' ' AS str LIMIT 0;
SHOW CREATE TABLE allchars;

CREATE TABLE t1tmp (a INT NOT NULL);
DELIMITER $$;
FOR i IN 0..0xFFF
DO
  INSERT INTO t1tmp VALUES (i);
END FOR;
$$
DELIMITER ;$$
INSERT INTO allchars SELECT
  t1.a*0x1000+t2.a,
  CHAR(t1.a*0x1000+t2.a USING utf32)
FROM t1tmp t1, t1tmp t2
WHERE t1.a BETWEEN 0 AND 0x10F;
DROP TABLE t1tmp;
SELECT COUNT(*) FROM allchars;
