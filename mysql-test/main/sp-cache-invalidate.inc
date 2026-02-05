--echo # sp-cache-invalidate
--disable_query_log
DELIMITER $$;
CREATE FUNCTION dummy() RETURNS TEXT
BEGIN
  RETURN 'dummy';
END;
$$
DELIMITER ;$$
DROP FUNCTION dummy;
--enable_query_log
