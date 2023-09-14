# Fetch one value from an open SYS_REFCURSOR

SET @sql_mode=@@sql_mode;

SET sql_mode=ORACLE;
DELIMITER /;
CREATE FUNCTION fetch_one_value(c SYS_REFCURSOR) RETURN VARCHAR AS
  v VARCHAR(128) :='none';
BEGIN
  IF c%ISOPEN THEN
    FETCH c INTO v;
  END IF;
  RETURN v;
END;
/
DELIMITER ;/
SET sql_mode=@sql_mode;
