--disable_query_log

DELIMITER $$;
CREATE PROCEDURE test_strnxfrm(nweights INT, maxsz INT)
BEGIN
  -- DECLARE flags INT DEFAULT 0x00000040/*MY_STRXFRM_PAD_WITH_SPACE*/ |
  --                           0x00000080/*MY_STRXFRM_PAD_TO_MAXLEN*/;
  DECLARE cl VARCHAR(64) DEFAULT (SELECT COLLATION_NAME
                                  FROM INFORMATION_SCHEMA.COLUMNS WHERE
                                  TABLE_NAME='t1' and COLUMN_NAME='str');
  DECLARE mul INT DEFAULT CASE
                            WHEN cl LIKE '%_as_cs' THEN 3
                            ELSE 1
                          END;
  DECLARE v1_def TEXT DEFAULT
   'CREATE VIEW v1_tmp AS SELECT '
   ' seq, str, '
   ' strnxfrm_source_length_used(str,seq,nweights,0xC0) AS srcused, '
   ' strnxfrm_warnings(str,seq,nweights,0xC0) AS warn, '
   ' RPAD(HEX(str),16) AS h, '
   ' hex(strnxfrm(str,seq,nweights,0xC0)) AS ws '
   'FROM t1, seq '
   'GROUP BY BINARY str, seq';

  CREATE TABLE seq (seq INT);
  FOR i IN 1..maxsz
  DO
    INSERT INTO seq VALUES (i);
  END FOR;

  EXECUTE IMMEDIATE REPLACE(v1_def, 'nweights', nweights);

  SELECT mul;

  SELECT seq, str, srcused, warn, h,
         CONCAT(ws,
                CASE WHEN warn & 2 THEN '^'
                     WHEN warn & 1 THEN '_'
                     ELSE ''
                END) AS ws
  FROM v1_tmp;

  DROP VIEW v1_tmp;

  DROP TABLE seq;
END;
$$
DELIMITER ;$$

DELIMITER $$;
CREATE PROCEDURE populate_t1(test VARCHAR(64))
BEGIN
  -- U+FEFF ZERO WIDTH NO-BREAK SPACE (an ignorable character)
  DECLARE zwnbsp TEXT CHARACTER SET utf8mb4 DEFAULT 0xEFBBBF;
  DECLARE fwA TEXT CHARACTER SET utf8mb4 DEFAULT 0xEFBCA1;
  CREATE TEMPORARY TABLE t0 (a TEXT CHARACTER SET utf8mb4);
  IF test='simple'
  THEN
    INSERT INTO t0 VALUES (''),('a');
    INSERT INTO t1 SELECT CONCAT(t1.a, t2.a) AS str FROM t0 t1, t0 t2;
  END IF;
  IF test='gb2312' OR test='big5'
  THEN
    INSERT INTO t0 VALUES (''),('a'),(fwA);
    INSERT INTO t1 SELECT CONCAT(t1.a, t2.a) AS str FROM t0 t1, t0 t2;
  END IF;
  IF test='uca'
  THEN
    INSERT INTO t0 VALUES (''),('c'),('h'),('ß');
    INSERT INTO t0 VALUES (CONCAT('c',zwnbsp));
    INSERT INTO t0 VALUES (CONCAT(zwnbsp,'c'));
    INSERT INTO t1 SELECT CONCAT(t1.a, t2.a) AS str FROM t0 t1, t0 t2;
  END IF;
  DROP TEMPORARY TABLE t0;
END;
$$
DELIMITER ;$$

--enable_query_log
