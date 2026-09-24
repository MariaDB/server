CREATE TABLE cmp_result (
  cl1 VARCHAR(64), cl2 VARCHAR(64), is_error INT, is_error_expected INT, expected_reason TEXT
) ENGINE=MyISAM;

CREATE TABLE cmp_result_all AS SELECT *, SPACE(32) AS repertoire FROM cmp_result LIMIT 0;


# Unicode collations that reorder ASCII letters
DELIMITER $$;
CREATE FUNCTION unicode_reorders_ascii_letters(cl VARCHAR(64)) RETURNS BOOL
BEGIN

  /* Old collations */
  CASE
  WHEN cl RLIKE '^(utf8mb3|ucs2|utf8mb4|utf16|utf32)'
                '_(czech|croatian|croatian_mysql561|danish|estonian|latvian|lithuanian|roman|slovak|spanish2|turkish)'
                '_ci$'
    THEN RETURN 1;

  /* MySQL 0900 compatibility collations */
  WHEN cl RLIKE '^utf8mb4'
                '_(es_trad|cs|et|la|lt|lv|sk|tr)'
                '_0900_ai_(ci|cs)$'
    THEN RETURN 1;

  /* uca1400 collations*/
  WHEN cl RLIKE '^(utf8mb3|ucs2|utf8mb4|utf16|utf32)'
                '_uca1400'
                '_(czech|croatian|danish|estonian|latvian|lithuanian|roman|slovak|spanish2|turkish)'
                '_(|nopad_)'
                '(ai_cs|ai_ci|as_cs|as_ci)$'
    THEN RETURN 1;
  ELSE
    RETURN 0;
  END CASE;

  RETURN 0;
END;
$$
DELIMITER ;$$


DELIMITER $$;
CREATE PROCEDURE update_expected_reasons(repertoire VARCHAR(32))
BEGIN
  UPDATE cmp_result SET is_error_expected=0, expected_reason='000 cl1 = cl2'  WHERE is_error_expected IS NULL AND cl1=cl2;

  UPDATE cmp_result SET is_error_expected=0, expected_reason='001 LEFT binary'  WHERE is_error_expected IS NULL AND cl1='binary' AND cl2<>'binary';
  UPDATE cmp_result SET is_error_expected=0, expected_reason='001 RIGHT binary' WHERE is_error_expected IS NULL AND cl1<>'binary' AND cl2='binary';

  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 RIGHT sameCS_bin' WHERE is_error_expected IS NULL AND cl1 NOT LIKE '%_bin' AND concat(substring_index(cl1,'_',1),'_bin')=cl2;
  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 RIGHT sameCS_bin' WHERE is_error_expected IS NULL AND cl1 NOT LIKE '%_bin' AND concat(substring_index(cl1,'_',1),'_nopad_bin')=cl2;
  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 RIGHT sameCS_bin' WHERE is_error_expected IS NULL AND cl1 NOT LIKE '%_bin' AND concat(substring_index(cl1,'_',1),'_0900_bin')=cl2;
  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 LEFT sameCS_bin' WHERE is_error_expected IS NULL AND cl2 NOT LIKE '%_bin' AND cl1=concat(substring_index(cl2,'_',1),'_bin');
  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 LEFT sameCS_bin' WHERE is_error_expected IS NULL AND cl2 NOT LIKE '%_bin' AND cl1=concat(substring_index(cl2,'_',1),'_nopad_bin');
  UPDATE cmp_result SET is_error_expected=0, expected_reason='002 LEFT sameCS_bin' WHERE is_error_expected IS NULL AND cl2 NOT LIKE '%_bin' AND cl1=concat(substring_index(cl2,'_',1),'_0900_bin');

  UPDATE cmp_result SET is_error_expected=0, expected_reason='003 LEFT superset'  WHERE is_error_expected IS NULL AND cl1 RLIKE '^(utf8mb4).*' AND cl2 NOT RLIKE '^(ucs2|utf8mb4|utf32|utf16).*';
  UPDATE cmp_result SET is_error_expected=0, expected_reason='003 RIGHT superset' WHERE is_error_expected IS NULL AND cl1 NOT RLIKE '^(ucs2|utf8mb4|utf32|utf16).*' AND cl2 RLIKE '^(utf8mb4).*';
  UPDATE cmp_result SET is_error_expected=0, expected_reason='003 LEFT superset'  WHERE is_error_expected IS NULL AND cl1 RLIKE '^(utf16|utf32).*' AND cl2 NOT RLIKE '^(ucs2|utf8mb3|utf8mb4|utf32|utf16).*';
  UPDATE cmp_result SET is_error_expected=0, expected_reason='003 RIGHT superset' WHERE is_error_expected IS NULL AND cl1 NOT RLIKE '^(ucs2|utf8mb3|utf8mb4|utf32|utf16).*' AND cl2 RLIKE '^(utf16|utf32).*';

  UPDATE cmp_result SET is_error_expected=0, expected_reason='004 LEFT superset'  WHERE is_error_expected IS NULL AND cl1 RLIKE '^(ucs2|utf8mb3).*' AND cl2 NOT RLIKE '^(utf8mb4|utf32|utf16|ucs2|utf8mb3).*';
  UPDATE cmp_result SET is_error_expected=0, expected_reason='004 RIGHT superset' WHERE is_error_expected IS NULL AND cl1 NOT RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2).*' AND cl2 RLIKE '^(ucs2|utf8mb3).*';


  IF repertoire ='ascii' THEN
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 sjis_bin reorders ASCII punctuation' WHERE is_error_expected IS NULL AND (cl1='sjis_bin' OR cl2='sjis_bin');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 sjis_nopad_bin reorders ASCII punctuation' WHERE is_error_expected IS NULL AND (cl1='sjis_nopad_bin' OR cl2='sjis_nopad_bin');
  END IF;

  IF repertoire ='ascii' THEN
    /* These collations have no tailoring on the entire ASCII repertoire */
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 no tailoring on the entire ASCII repertoire'
    WHERE is_error_expected IS NULL
      AND (cl1 RLIKE '^(big5_chinese|gb2312_chinese|gbk_chinese|sjis_japanese)' OR
           cl2 RLIKE '^(big5_chinese|gb2312_chinese|gbk_chinese|sjis_japanese)');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 hp8_english_ci has anomalies on ASCII punctuation'
    WHERE is_error_expected IS NULL AND (cl1='hp8_english_ci' OR cl2='hp8_english_ci');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 swe7_swedish_nopad_ci reorders ASCII punctuation'
    WHERE is_error_expected IS NULL AND (cl1='swe7_swedish_nopad_ci' OR cl2='swe7_swedish_nopad_ci');
  END IF;

  /*
    The ASCII digits are sorted in the code point order in all collations,
    regardless of the order of the letters and the case sensitivity.
    So on the digits repertoire only PAD vs NOPAD collations are incompatible.
  */
  IF repertoire NOT IN ('digits', 'minus_dot_digits') THEN
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 cp1257_lithuanian_ci reorders ASCII letters: I<Y<J' WHERE is_error_expected IS NULL AND  (cl1='cp1257_lithuanian_ci' OR cl2='cp1257_lithuanian_ci');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 latin5_turkish_ci reorders ASCII letters: I<i<J' WHERE is_error_expected IS NULL AND (cl1 RLIKE '^latin5_turkish' OR cl2 RLIKE '^latin5_turkish');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 cp866_general_ci reorders ASCII letters: J<j (bug)' WHERE is_error_expected IS NULL AND (cl1='cp866_general_ci' OR cl2='cp866_general_ci');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 macce_general_ci reorders ASCII letters: M=m=N=n' WHERE is_error_expected IS NULL AND (cl1='macce_general_ci' OR cl2='macce_general_ci');
    UPDATE cmp_result SET is_error_expected=1, expected_reason='010 latin7_general_ci reorders ASCII letters: t<T (bug)' WHERE is_error_expected IS NULL AND (cl1='latin7_general_ci' OR cl2='latin7_general_ci');

    UPDATE cmp_result SET is_error_expected=1, expected_reason='020 reorders ASCII letters' WHERE is_error_expected IS NULL AND unicode_reorders_ascii_letters(cl1)=TRUE;
    UPDATE cmp_result SET is_error_expected=1, expected_reason='020 reorders ASCII letters' WHERE is_error_expected IS NULL AND unicode_reorders_ascii_letters(cl2)=TRUE;

    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS cs vs bin' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_cs$' AND cl2 RLIKE '_bin$';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS cs vs ci' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_cs$' AND cl2 RLIKE '_ci$';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS ci vs cs' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_ci$' AND cl2 RLIKE '_cs$';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS bin vs cs' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_bin$' AND cl2 RLIKE '_cs$';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS ci vs bin' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_ci$' AND cl2 RLIKE '_bin$';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='030 diffCS bin vs ci' WHERE is_error_expected IS NULL AND  cl1 RLIKE '_bin$' AND cl2 RLIKE '_ci$';
  END IF;

  UPDATE cmp_result SET is_error_expected=1, expected_reason='030 nopad vs pad' WHERE is_error_expected IS NULL AND cl1 RLIKE '_(nopad|0900)_' AND cl2 NOT RLIKE '_(nopad|0900)_';
  UPDATE cmp_result SET is_error_expected=1, expected_reason='030 pad vs nopad' WHERE is_error_expected IS NULL AND cl1 NOT RLIKE '_(nopad|0900)_' AND cl2 RLIKE '_(nopad|0900)_';

  /*
    tis620_thai_ci and tis620_thai_nopad_ci fold letters to lower case,
    so they sort the underscore before the letters.
    They are compatible with other collations only on the ALNUM repertoire.
  */
  IF repertoire IN ('ident','ascii') THEN
    UPDATE cmp_result SET is_error_expected=1, expected_reason='040 tis620_thai_ci sorts underscore before letters'
    WHERE is_error_expected IS NULL
      AND ((cl1 RLIKE '^tis620_thai' AND cl2 NOT RLIKE '^tis620_thai')
        OR (cl2 RLIKE '^tis620_thai' AND cl1 NOT RLIKE '^tis620_thai'));
  END IF;

  /*
    Unicode _general_ vs UCA collations
    have different position of the underscore
  */
  IF repertoire IN ('ident','ascii') THEN
    UPDATE cmp_result SET is_error_expected=1, expected_reason='040 different underscore position'
    WHERE is_error_expected IS NULL
      AND cl1 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_general.*'
      AND cl2 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(unicode|swedish|czech|uca).*';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='040 different underscore position'
    WHERE is_error_expected IS NULL
      AND cl1 RLIKE  '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(unicode|swedish|czech|uca).*'
      AND cl2 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(general).*';
  END IF;

  /*
    Old UCA vs uca1400 collations have different punctuation order.
    The minus and the dot are punctuation characters, so the UCA versions
    must be equal on the repertoire with them.
  */
  IF repertoire IN ('minus_dot_digits','ascii') THEN
    UPDATE cmp_result SET is_error_expected=1, expected_reason='050 different punctuation order'
    WHERE is_error_expected IS NULL
      AND cl1 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(unicode|swedish|czech).*'
      AND cl2 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(uca1400|0900).*';
    UPDATE cmp_result SET is_error_expected=1, expected_reason='050 different punctuation order'
    WHERE is_error_expected IS NULL
      AND cl1 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(uca1400|0900).*'
      AND cl2 RLIKE '^(utf8mb4|utf32|utf16|utf8mb3|ucs2)_(unicode|swedish|czech).*';
  END IF;

  /*
    All other pairs are expected to be compatible: the collations
    have equal tailorings on the repertoire.
    Without this update is_error_expected would stay NULL for these pairs,
    and the check "is_error<>is_error_expected" would skip them.
  */
  UPDATE cmp_result SET is_error_expected=0, expected_reason='100 equal tailoring' WHERE is_error_expected IS NULL;

END;
$$
DELIMITER ;$$



DELIMITER $$;
CREATE PROCEDURE check_compatibility(repertoire VARCHAR(32), param VARCHAR(32))
BEGIN
  DECLARE mask VARCHAR(32) DEFAULT COALESCE(@mask,'');
  DECLARE errmsg TEXT;
  DECLARE c1 CURSOR FOR SELECT full_collation_name
                        FROM collations
                        ORDER BY full_collation_name;
  DECLARE c2 CURSOR FOR SELECT full_collation_name
                        FROM collations
                        ORDER BY full_collation_name;

  DECLARE CONTINUE HANDLER FOR 1267 /*Illegal mix of collations*/
  BEGIN
    GET DIAGNOSTICS CONDITION 1 errmsg = MESSAGE_TEXT;
  END;

  TRUNCATE TABLE cmp_result;
  FOR rec1 IN c1 DO
    SET @@collation_connection=rec1.full_collation_name;
    EXECUTE IMMEDIATE REPLACE('CREATE VIEW v1 AS '
                              'SELECT ''PARAM'' AS alnum', 'PARAM', param);
    SET NAMES DEFAULT;
    FOR rec2 IN c2 DO
      SET errmsg= NULL;
      SET @@collation_connection=rec2.full_collation_name;
      /* This query raises an error on incompatible collations */
      IF (rec1.full_collation_name RLIKE mask OR
          rec2.full_collation_name RLIKE mask)
      THEN
        SET @a= NULL;
        EXECUTE IMMEDIATE
          REPLACE('SET @a=(SELECT COUNT(*) FROM v1 WHERE v1.alnum=''PARAM'')',
                  'PARAM', param);
        /*
          is_error is set to 1 if the query raised an error, and to 2 if it
          did not raise an error but did not return the expected single row.
          So a wrong result is reported by "is_error<>is_error_expected",
          as is_error_expected can be only 0 or 1.
        */
        INSERT INTO cmp_result (cl1, cl2, is_error)
          VALUES (rec1.full_collation_name,
                  rec2.full_collation_name,
                  IF(errmsg IS NOT NULL, 1, IF(@a=1, 0, 2)));
      END IF;
    END FOR;
    DROP VIEW v1;
  END FOR;
  SET NAMES DEFAULT;
  CALL update_expected_reasons(repertoire);
END;
$$
DELIMITER ;$$


CREATE TABLE collations (full_collation_name VARCHAR(64) NOT NULL PRIMARY KEY);
INSERT INTO collations VALUES
('latin1_general_cs'), ('latin5_turkish_ci'),
('latin1_swedish_nopad_ci'), ('swe7_swedish_nopad_ci'),
('utf8mb3_uca1400_nopad_ai_cs'),('utf8mb4_0900_as_cs'),
('latin1_nopad_bin'), ('swe7_nopad_bin'),
('latin1_german1_ci'),('latin1_swedish_ci'),
('utf8mb3_uca1400_ai_cs'),('utf32_uca1400_as_cs'),
('latin1_bin'),('latin5_bin'),
('hp8_english_ci');

INSERT INTO collations VALUES
('utf8mb3_general_ci'),('utf8mb3_general_nopad_ci'),
('utf8mb3_unicode_ci'),('utf8mb3_unicode_nopad_ci'),
('utf8mb3_bin'),('utf8mb3_nopad_bin'),
('utf8mb4_general_ci'),('utf8mb4_general_nopad_ci'),
('utf8mb4_uca1400_ai_ci'),('utf8mb4_uca1400_nopad_ai_ci'),
('utf8mb4_unicode_ci'),('utf8mb4_unicode_nopad_ci'),
('utf8mb4_bin'),('utf8mb4_nopad_bin');


DELIMITER $$;
BEGIN NOT ATOMIC
  IF @extra_collations_query IS NOT NULL
  THEN
    EXECUTE IMMEDIATE @extra_collations_query;
  END IF;
END;
$$
DELIMITER ;$$


--echo #
--echo # ASCII digits repertoire
--echo #

CALL check_compatibility('digits', '12');
SELECT * FROM cmp_result WHERE is_error<>is_error_expected;
SELECT
   RPAD(cl1, 32) AS cl1, RPAD(cl2, 32) AS cl2,
   is_error, is_error_expected, expected_reason
FROM cmp_result ORDER BY expected_reason,cl1, cl2;
INSERT INTO cmp_result_all SELECT *, '0_digits' FROM cmp_result;


--echo #
--echo # ASCII minus, dot and digits repertoire
--echo #

CALL check_compatibility('minus_dot_digits', '-1.5');
SELECT * FROM cmp_result WHERE is_error<>is_error_expected;
SELECT
   RPAD(cl1, 32) AS cl1, RPAD(cl2, 32) AS cl2,
   is_error, is_error_expected, expected_reason
FROM cmp_result ORDER BY expected_reason,cl1, cl2;
INSERT INTO cmp_result_all SELECT *, '1_minus_dot_digits' FROM cmp_result;


--echo #
--echo # ALNUM repertoire
--echo #

CALL check_compatibility('alnum', 'a1');
SELECT * FROM cmp_result WHERE is_error<>is_error_expected;
SELECT
   RPAD(cl1, 32) AS cl1, RPAD(cl2, 32) AS cl2,
   is_error, is_error_expected, expected_reason
FROM cmp_result ORDER BY expected_reason,cl1, cl2;
INSERT INTO cmp_result_all SELECT *, 'A_alnum' FROM cmp_result;


--echo #
--echo # IDENT repertoire
--echo #

CALL check_compatibility('ident', 'a_');
SELECT * FROM cmp_result WHERE is_error<>is_error_expected;
SELECT
   RPAD(cl1, 32) AS cl1, RPAD(cl2, 32) AS cl2,
   is_error, is_error_expected, expected_reason
FROM cmp_result ORDER BY expected_reason,cl1, cl2;
INSERT INTO cmp_result_all SELECT *, 'B_ident' FROM cmp_result;


--echo #
--echo # Entire ASCII-7 repertoire
--echo #

CALL check_compatibility('ascii', 'a#');
SELECT * FROM cmp_result WHERE is_error<>is_error_expected;
SELECT
   RPAD(cl1, 32) AS cl1, RPAD(cl2, 32) AS cl2,
   is_error, is_error_expected, expected_reason
FROM cmp_result ORDER BY expected_reason,cl1, cl2;
INSERT INTO cmp_result_all SELECT *, 'C_ascii' FROM cmp_result;


--echo #
--echo # Collation pairs having different results on different repertoires
--echo #

SELECT
  RPAD(cl1,32) AS cl1, RPAD(cl2,32) AS cl2,
  GROUP_CONCAT(repertoire, is_error ORDER BY repertoire) AS is_error_all
FROM cmp_result_all
GROUP BY cmp_result_all.cl1, cmp_result_all.cl2
HAVING is_error_all NOT IN
  ('0_digits0,1_minus_dot_digits0,A_alnum0,B_ident0,C_ascii0',
   '0_digits1,1_minus_dot_digits1,A_alnum1,B_ident1,C_ascii1')
ORDER BY is_error_all;


DROP TABLE cmp_result_all;
DROP TABLE cmp_result;
DROP TABLE collations;
DROP PROCEDURE check_compatibility;
DROP PROCEDURE update_expected_reasons;
DROP FUNCTION unicode_reorders_ascii_letters;
SET @extra_collations_query=NULL;
SET @mask=NULL;
