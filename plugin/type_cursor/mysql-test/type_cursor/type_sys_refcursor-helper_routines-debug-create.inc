--echo #
--echo # Helper routines
--echo #

# Return a string with ref counters for cursors in the given range,
# with format '[ cnt0 cnt1 cnt2 cnt 3 cnt4 cnt5 ]', for example:
#  '[  2  1  NULL NULL NULL]'

DELIMITER /;
CREATE FUNCTION refs(first INT, last INT) RETURNS TEXT
BEGIN
  DECLARE res TEXT DEFAULT '[';
  FOR i IN first..last
  DO
    SET res= CONCAT(res, COALESCE(CURSOR_REF_COUNT(i), 'NULL'));
    IF i < last THEN
      SET res= CONCAT(res, '\t');
    END IF;
  END FOR;
  SET res= CONCAT(res, ']');
  RETURN res;
END;
/
DELIMITER ;/

# Show a cursor and ref counters in the given range
DELIMITER /;
CREATE PROCEDURE show_cursor_and_refs(stage VARCHAR(32),
                                      curs VARCHAR(32),
                                      first INT, last INT)
BEGIN
  SELECT stage, COALESCE(curs, 'NULL') AS curs, refs(first, last) AS refs;
END;
/
DELIMITER ;/


# Returns a new open cursor with logging
DELIMITER /;
CREATE FUNCTION ff0() RETURNS SYS_REFCURSOR
BEGIN
  DECLARE c0 SYS_REFCURSOR;
  IF @log <> '' THEN
    SET @log= CONCAT(@log, '\n');
  END IF;
  SET @log= CONCAT(@log, 'ff0-0','\t',
                     COALESCE(CAST(c0 AS CHAR),'NULL'), '\t',
                     refs(0,5), '\n');
  OPEN c0 FOR SELECT 10; 
  SET @log= CONCAT(@log, 'ff0-1','\t',
                     COALESCE(CAST(c0 AS CHAR),'NULL'), '\t',
                     refs(0,5));
  RETURN c0;
END;
/
DELIMITER ;/
SHOW FUNCTION CODE ff0;


# Returns an existing cursor passed to the IN parameter, with logging
DELIMITER /;
CREATE FUNCTION ff1(c0 SYS_REFCURSOR) RETURNS SYS_REFCURSOR
BEGIN
  IF @log <> '' THEN
    SET @log= CONCAT(@log, '\n');
  END IF;
  SET @log= CONCAT(@log, 'ff1-0','\t',
                     COALESCE(CAST(c0 AS CHAR),'NULL'), '\t',
                     refs(0,5));
  RETURN c0;
END;
/
DELIMITER ;/
SHOW FUNCTION CODE ff1;
