--echo # Start of allowed.inc

SELECT CURRENT_USER;

--echo #
--echo # $acl + $user + same DB
--echo #

USE test1;

--echo # $acl + $user + same DB + PACKAGE type in the same PACKAGE BODY

# Does not work !!!
#DELIMITER $$;
#CREATE PACKAGE BODY pkg1 AS
#  r0 rec0_t;
#END;
#$$
#DELIMITER ;$$

DELIMITER $$;
CREATE PACKAGE BODY pkg1 AS
  r0 pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE BODY pkg1;

DELIMITER $$;
CREATE PACKAGE BODY pkg1 AS
  r0 test1.pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE BODY pkg1;

--echo # $acl + $user + same DB + PACKAGE type in another PACKAGE

DELIMITER $$;
CREATE PACKAGE pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;

DELIMITER $$;
CREATE PACKAGE pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;

--echo # $acl + $user + same DB + PACKAGE type in another PACKAGE BODY

DELIMITER $$;
CREATE PACKAGE pkg2 AS
END;
$$
CREATE PACKAGE BODY pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;

DELIMITER $$;
CREATE PACKAGE pkg2 AS
END;
$$
CREATE PACKAGE BODY pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;


--echo # $acl + $user + same DB + PACKAGE type in a schema routine

DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE cur1_t IS REF CURSOR RETURN pkg1.rec0_t;
BEGIN
  NULL;
END;
$$
DELIMITER ;$$
DROP PROCEDURE p1;

DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
BEGIN
  NULL;
END;
$$
DELIMITER ;$$
DROP PROCEDURE p1;


--echo #
--echo # $acl + $user + different DB
--echo #

USE test2;

--echo # $acl + $user + different DB + PACKAGE type in another PACKAGE

DELIMITER $$;
CREATE PACKAGE pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;

--echo # $acl + $user + different DB + PACKAGE type in another PACKAGE BODY

DELIMITER $$;
CREATE PACKAGE pkg2 AS
END;
$$
CREATE PACKAGE BODY pkg2 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
END;
$$
DELIMITER ;$$
DROP PACKAGE pkg2;


--echo # $acl + $user + different DB + PACKAGE type in a schema routine

DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE cur1_t IS REF CURSOR RETURN test1.pkg1.rec0_t;
BEGIN
  NULL;
END;
$$
DELIMITER ;$$
DROP PROCEDURE p1;

--echo # End of allowed.inc
