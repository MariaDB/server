--disable_ps_protocol
--enable_metadata
DELIMITER $$;
FOR rec IN (SELECT COLLATION_NAME
            FROM INFORMATION_SCHEMA.COLLATION_CHARACTER_SET_APPLICABILITY
            WHERE CHARACTER_SET_NAME=@charset
              AND COLLATION_NAME RLIKE 'uca1400'
            ORDER BY ID)
DO
  SET NAMES utf8mb4;
  SET character_set_results=NULL;
  EXECUTE IMMEDIATE CONCAT('SELECT CONVERT('''' USING ',@charset,')',
                           ' COLLATE ', rec.COLLATION_NAME,
                           ' AS ', rec.COLLATION_NAME,
                           ' LIMIT 0');
END FOR;
$$
DELIMITER ;$$
--disable_metadata
--enable_ps_protocol
SET NAMES utf8;
