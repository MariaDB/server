--source include/have_utf32.inc
--source include/have_sequence.inc

EXECUTE IMMEDIATE SFORMAT('
CREATE VIEW v_bmp AS
SELECT
  seq AS codepoint,
  LPAD(HEX(seq),4,''0'') AS codepoint_hex4,
  CONVERT(CHAR(seq USING utf32) USING {}) COLLATE {} AS c
FROM
  seq_0_to_65535', @@character_set_connection, @@collation_connection);

--disable_service_connection
SELECT COLLATION(c) FROM v_bmp LIMIT 1;

SELECT HEX(codepoint) FROM v_bmp WHERE HEX(WEIGHT_STRING(c))='FFFD';

SELECT
  SUM(codepoint_hex4=HEX(WEIGHT_STRING(c))) AS count_bmp_weight_is_codepoint,
  SUM(codepoint_hex4<>HEX(WEIGHT_STRING(c))) AS count_bmp_weight_is_not_codepoint
FROM v_bmp;

SELECT codepoint_hex4,HEX(WEIGHT_STRING(c))
FROM v_bmp
WHERE codepoint_hex4<>HEX(WEIGHT_STRING(c));

--enable_service_connection

DROP VIEW v_bmp;
