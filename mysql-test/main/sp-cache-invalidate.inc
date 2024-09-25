--echo # sp-cache-invalidate
--disable_query_log
DELIMITER $$;
CREATE FUNCTION dummy() RETURNS INT
BEGIN
  RETURN 1;
END;
$$
DELIMITER ;$$
DROP FUNCTION dummy;
--enable_query_log
