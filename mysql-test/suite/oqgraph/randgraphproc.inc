# SQL implementation of randsocial.c for OQGRAPH
# Copyright (C) by Arjen Lentz <arjen@openquery.com> for Open Query
# GPL v2+ licensed with the rest of OQGRAPH
# for use in mysql-test
# 2013-03-01 first implementation based on randsocial.c in old oqgraph v2 tree

delimiter |;
--disable_warnings
DROP PROCEDURE IF EXISTS randnotx|
--enable_warnings
CREATE PROCEDURE randnotx (INOUT rseed INT, IN items INT, IN x INT, OUT rval INT)
BEGIN
  REPEAT
    # Simple LCG (BSD)
    SET rseed = (rseed * 1103515245 + 12345) & 2147483647;
    SET rval = ((rseed >> 16) & 32767) MOD items;
  UNTIL rval <> x
  END REPEAT;
END|


--disable_warnings
DROP PROCEDURE IF EXISTS randgraphproc|
--enable_warnings
# this procedure is deterministic with its private seeded random generator
CREATE PROCEDURE randgraphproc (IN items INT, IN friends INT, IN fanof INT, IN maxweight INT)
BEGIN
  DECLARE i,j,weight,rseed,rval INT;

  # we need repeatability even in multi-threading
  SET rseed = items;

  SET i = 0;
  WHILE i < items DO
    SET j = 0;
    WHILE j < (friends + fanof) DO
      CALL randnotx(rseed,items,i,rval);

      IF (maxweight > 0) THEN
        # Use -1 as we don't need to exclude anything
        CALL randnotx(rseed,items,-1,weight);
        SET weight = weight MOD maxweight;
      ELSE
        SET weight = 0;
      END IF;

#      SELECT CONCAT('INSERT IGNORE rsb VALUES (',i,',',rval,',',weight,');');
      INSERT IGNORE rsb VALUES (i,rval,weight);

      # only create backlinks for friends, not fans
      IF (j < friends) THEN
#        SELECT CONCAT('INSERT IGNORE rsb VALUES (',rval,',',i,',',weight,');');
        INSERT IGNORE rsb VALUES (rval,i,weight);
      END IF;

      SET j = j + 1;
    END WHILE;

    SET i = i + 1;
  END WHILE;
END|
DELIMITER ;;
