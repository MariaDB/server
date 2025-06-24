--source include/have_utf32.inc
--source include/have_sequence.inc

EXECUTE IMMEDIATE SFORMAT('
CREATE VIEW v_supplementary AS
SELECT
  seq AS codepoint,
  LPAD(HEX(seq),8,''0'') AS codepoint_hex8,
  CONVERT(CHAR(seq USING utf32) USING {}) COLLATE {} AS c
FROM
  seq_65536_to_1114111', @@character_set_connection, @@collation_connection);

SELECT COLLATION(c) FROM v_supplementary LIMIT 1;

SELECT
  codepoint_hex8,
  HEX(CAST(LOWER(c) AS CHAR CHARACTER SET utf32)),
  HEX(CAST(UPPER(c) AS CHAR CHARACTER SET utf32))
FROM v_supplementary
WHERE BINARY(c)<>BINARY(LOWER(c)) OR BINARY(c)<>BINARY(UPPER(c));

DROP VIEW v_supplementary;
