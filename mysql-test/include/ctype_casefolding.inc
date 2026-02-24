CREATE OR REPLACE TABLE case_folding AS SELECT 0 AS code, SPACE(32) AS c LIMIT 0;
SHOW CREATE TABLE case_folding;
# Uncode code points that have a variable length case mapping in utf8
# (e.g.  LOWER('2-byte-character') -> '3-byte-character'
INSERT INTO case_folding (code) VALUES
(0x23A),
(0x23E),
(0x23F),
(0x240),
(0x250),
(0x251),
(0x252),
(0x26B),
(0x271),
(0x27D);

INSERT INTO case_folding (code) VALUES
(0x0049) /* LATIN CAPITAL LETTER I */,
(0x0069) /* LATIN SMALL LETTER I */,
(0x0130) /* LATIN CAPITAL LETTER I WITH DOT ABOVE */,
(0x0131) /* LATIN SMALL LETTER DOTLESS I */
;

INSERT INTO case_folding (code) VALUES
(0x2C2F) /* GLAGOLITIC CAPITAL LETTER CAUDATE CHRIVI (Unicode-14.0) */,
(0x2C5F) /* GLAGOLITIC SMALL LETTER CAUDATE CHRIVI (Unicode-14.0) */,
(0xA7C0) /* LATIN CAPITAL LETTER OLD POLISH O (Unicode-14.0) */,
(0xA7C1) /* LATIN SMALL LETTER OLD POLISH O (Unicode-14.0) */;

UPDATE case_folding SET c=CHAR(code USING ucs2);
SELECT HEX(code), HEX(LOWER(c)), HEX(UPPER(c)), c FROM case_folding;
DROP TABLE case_folding;
