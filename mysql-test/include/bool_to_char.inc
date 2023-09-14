SET @sql_mode=@@sql_mode;

SET sql_mode=ORACLE;

DELIMITER /;
CREATE FUNCTION bool_to_char(b BOOLEAN) RETURN VARCHAR AS
BEGIN
  RETURN
  CASE
     WHEN b IS NULL THEN 'NULL'
     WHEN b         THEN 'true'
     WHEN NOT b     THEN 'false'
  END;
END;
/
DELIMITER ;/

SET sql_mode=@sql_mode;
