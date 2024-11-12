--connection slave
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid= slave_pos;
--source include/start_slave.inc

--connection master
CREATE TABLE t1 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
INSERT INTO t1 VALUES (0, 0);

--echo *** Test looking up a lot of different event positions and GTIDs.

# A function for comparing GTID positions.
# Handles that the domain_id order is different in the two strings.
# Works by repeatedly removing one GTID from each string. If the strings have
# the same length and nothing is left at the end, then they are identical.
delimiter //;
CREATE FUNCTION gtid_eq(a VARCHAR(255), b VARCHAR(255)) RETURNS BOOLEAN DETERMINISTIC
BEGIN
  DECLARE g VARCHAR(255);
  IF a IS NULL OR b IS NULL OR LENGTH(a) != LENGTH(b) THEN
    RETURN FALSE;
  END IF;
  SET a= CONCAT(a, ',');
  SET b= CONCAT(',', b, ',');
  WHILE LENGTH(a) > 0 DO
    SET g= REGEXP_SUBSTR(a, '^[^,]+,');
    SET a= SUBSTRING(a, LENGTH(g)+1);
    SET b= REPLACE(b, CONCAT(',', g), ',');
  END WHILE;
  RETURN b = ',';
END //
delimiter ;//
