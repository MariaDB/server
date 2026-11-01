# The ASCII digits repertoire. It is compatible even with collations having
# anomalies on letters, but not with a collation with a different PAD/NOPAD.
# The result is printed instead of raising the error, to show both outcomes.
--echo # ASCII digits
--disable_query_log
DELIMITER $$;
BEGIN NOT ATOMIC
  DECLARE CONTINUE HANDLER FOR 1267 /*Illegal mix of collations*/
    SELECT 'ER_CANT_AGGREGATE_2COLLATIONS' AS d;
  SELECT d FROM v_tailoring WHERE d='1';
END;
$$
DELIMITER ;$$
--enable_query_log
--echo # ASCII minus, dot and digits
--disable_query_log
DELIMITER $$;
BEGIN NOT ATOMIC
  DECLARE CONTINUE HANDLER FOR 1267 /*Illegal mix of collations*/
    SELECT 'ER_CANT_AGGREGATE_2COLLATIONS' AS e;
  SELECT e FROM v_tailoring WHERE e='-1.5';
END;
$$
DELIMITER ;$$
--enable_query_log
SELECT c FROM v_tailoring WHERE c='Y';
SELECT c FROM v_tailoring WHERE c<>'Y_';
--error ER_CANT_AGGREGATE_2COLLATIONS
SELECT c FROM v_tailoring WHERE c<>'Y+';
