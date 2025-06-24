--source include/have_utf32.inc
--source include/have_ucs2.inc
--source include/have_sequence.inc


EXECUTE IMMEDIATE SFORMAT('
CREATE VIEW v_bmp AS
SELECT
  seq AS codepoint,
  LPAD(HEX(seq),4,''0'') AS codepoint_hex4,
  CONVERT(CHAR(seq USING utf32) USING {}) COLLATE {} AS c
FROM
  seq_0_to_65535', @@character_set_connection, @@collation_connection);

SELECT COLLATION(c) FROM v_bmp LIMIT 1;

--echo #
--echo # BMP character summary
--echo #

SELECT
  BINARY(c)=BINARY(LOWER(c)) AS `Bc=BLc`,
  BINARY(c)=BINARY(UPPER(c)) AS `Bc=BUc`,
  c=LOWER(c) AS `c=L(c)`,
  c=UPPER(c) AS `c=U(c)`,
  c LIKE LOWER(c) AS `c~~L(c)`,
  c LIKE UPPER(c) AS `c~~U(c)`,
  COUNT(*),
  IF(BINARY(c)=BINARY(LOWER(c)) AND BINARY(c)=BINARY(UPPER(c)),'',
     LEFT(GROUP_CONCAT(c ORDER BY codepoint), 20)) AS example
FROM v_bmp
GROUP BY 1, 2, 3, 4, 5, 6;


--echo #
--echo # BMP characters with upper/lower mapping
--echo #

SELECT
  codepoint_hex4,
  HEX(CAST(LOWER(c) AS CHAR CHARACTER SET ucs2)),
  HEX(CAST(UPPER(c) AS CHAR CHARACTER SET ucs2))
FROM v_bmp
WHERE BINARY(c)<>BINARY(LOWER(c)) OR BINARY(c)<>BINARY(UPPER(c));

--echo #
--echo # BMP characters with a non-trivial upper/lower mapping
--echo #

SELECT
  codepoint_hex4 as hex4,
  HEX(CAST(LOWER(c) AS CHAR CHARACTER SET ucs2)) AS hex4_l,
  HEX(CAST(UPPER(c) AS CHAR CHARACTER SET ucs2)) AS hex4_u,
  c=LOWER(c) AS `c=L`,
  c=UPPER(c) AS `c=U`,
  c LIKE LOWER(c) AS `c~~L`,
  c LIKE UPPER(c) AS `c~~U`,
  c,
  LOWER(c) AS `L(c)`,
  UPPER(c) AS `U(c)`
FROM v_bmp
WHERE NOT (
  (BINARY(c)=BINARY(LOWER(c)) OR BINARY(c)=BINARY(UPPER(c))) AND
  c = LOWER(c) AND
  c = UPPER(c) AND
  c LIKE UPPER(c) AND
  c LIKE LOWER(c)
);


DROP VIEW v_bmp;
