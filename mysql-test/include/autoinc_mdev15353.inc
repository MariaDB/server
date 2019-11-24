DELIMITER $$;
CREATE PROCEDURE autoinc_mdev15353_one(engine VARCHAR(64), t VARCHAR(64))
BEGIN
  DECLARE query TEXT DEFAULT 'CREATE TABLE t1 ('
                             '  id TTT NOT NULL AUTO_INCREMENT,'
                             '  name CHAR(30) NOT NULL,'
                             '  PRIMARY KEY (id)) ENGINE=EEE';
  EXECUTE IMMEDIATE REPLACE(REPLACE(query,'TTT', t), 'EEE', engine);
  SHOW CREATE TABLE t1;
  INSERT INTO t1 (name) VALUES ('dog');
  SELECT * FROM t1;
  UPDATE t1 SET id=-1 WHERE id=1;
  SELECT * FROM t1;
  INSERT INTO t1 (name) VALUES ('cat');
  SELECT * FROM t1;
  DROP TABLE t1;
END;
$$
DELIMITER ;$$

CALL autoinc_mdev15353_one(@engine, 'tinyint');
CALL autoinc_mdev15353_one(@engine, 'smallint');
CALL autoinc_mdev15353_one(@engine, 'mediumint');
CALL autoinc_mdev15353_one(@engine, 'int');
CALL autoinc_mdev15353_one(@engine, 'bigint');
CALL autoinc_mdev15353_one(@engine, 'float');
CALL autoinc_mdev15353_one(@engine, 'double');

DROP PROCEDURE autoinc_mdev15353_one;
