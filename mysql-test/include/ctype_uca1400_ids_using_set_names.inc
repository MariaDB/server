
--disable_ps_protocol
--enable_metadata
DELIMITER $$;
FOR rec IN (SELECT COLLATION_NAME
            FROM INFORMATION_SCHEMA.COLLATION_CHARACTER_SET_APPLICABILITY
            WHERE CHARACTER_SET_NAME=@charset
              AND COLLATION_NAME RLIKE 'uca1400'
            ORDER BY ID)
DO
  EXECUTE IMMEDIATE CONCAT('SET NAMES ',@charset,' COLLATE ', rec.COLLATION_NAME);
  SELECT rec.COLLATION_NAME;
END FOR;
$$
DELIMITER ;$$
--disable_metadata
--enable_ps_protocol
