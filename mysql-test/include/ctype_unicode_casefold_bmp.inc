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

SELECT
  codepoint_hex4,
  HEX(CAST(LOWER(c) AS CHAR CHARACTER SET ucs2)),
  HEX(CAST(UPPER(c) AS CHAR CHARACTER SET ucs2))
FROM v_bmp
WHERE BINARY(c)<>BINARY(LOWER(c)) OR BINARY(c)<>BINARY(UPPER(c));

DROP VIEW v_bmp;
