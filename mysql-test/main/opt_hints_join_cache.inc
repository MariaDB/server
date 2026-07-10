EXPLAIN  SELECT * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);
--echo --------------------------------------------
EXPLAIN  SELECT /*+ BKA() */ * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);
--echo --------------------------------------------
EXPLAIN  SELECT /*+ BNL() */ * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);
--echo --------------------------------------------
EXPLAIN  SELECT /*+ NO_BKA() */ * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);
--echo --------------------------------------------
EXPLAIN  SELECT /*+ NO_BNL() */ * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);
--echo --------------------------------------------
EXPLAIN  SELECT /*+ NO_BKA() NO_BNL() */ * FROM t12, t13
  WHERE t12.a=t13.a AND (t13.b+1 <= t12.b+1);

