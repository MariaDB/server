--source include/have_utf32.inc
--source include/have_sequence.inc

EXECUTE IMMEDIATE SFORMAT('
CREATE VIEW v_supplementary AS
SELECT
  seq AS codepoint,
  CONVERT(CHAR(seq USING utf32) USING {}) COLLATE {} AS c
FROM
  seq_65536_to_1114111', @@character_set_connection, @@collation_connection);

--disable_service_connection
SELECT COLLATION(c) FROM v_supplementary LIMIT 1;

SELECT
  SUM(HEX(WEIGHT_STRING(c))<>'FFFD'),
  SUM(HEX(WEIGHT_STRING(c))='FFFD')
FROM v_supplementary;
--enable_service_connection

DROP VIEW v_supplementary;
