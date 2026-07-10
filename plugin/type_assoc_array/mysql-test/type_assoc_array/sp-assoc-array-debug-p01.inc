DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE person_t IS RECORD
  (
    field_name VARCHAR(100),
    last_name VARCHAR(100)
  );
  TYPE assoc_t IS TABLE OF person_t INDEX BY VARCHAR(20);
  assoc assoc_t;
BEGIN
  NULL;
END;
$$
DELIMITER ;$$
CALL p1;
DROP PROCEDURE p1;
