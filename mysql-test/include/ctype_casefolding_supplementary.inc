CREATE OR REPLACE TABLE case_folding AS SELECT 0 AS code, SPACE(32) AS c, SPACE(64) AS comment LIMIT 0;
SHOW CREATE TABLE case_folding;

INSERT INTO case_folding (code, comment) VALUES (0x10595, 'VITHKUQI CAPITAL LETTER ZE (Unicode-14.0)');
INSERT INTO case_folding (code, comment) VALUES (0x105BC, 'VITHKUQI SMALL LETTER ZE (Unicode-14.0)');
INSERT INTO case_folding (code, comment) VALUES (0x1E921, 'ADLAM CAPITAL LETTER SHA (Unicode-9.0)');
INSERT INTO case_folding (code, comment) VALUES (0x1E943, 'ADLAM SMALL LETTER SHA (Unicode-9.0)');

UPDATE case_folding SET c=CHAR(code USING utf32);
SELECT
  HEX(CONVERT(c USING utf32)) AS ch,
  HEX(CONVERT(LOWER(c) USING utf32)) AS cl,
  HEX(CONVERT(UPPER(c) USING utf32)) AS cu,
  comment
FROM case_folding ORDER BY BINARY(c);
DROP TABLE case_folding;
