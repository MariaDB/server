SELECT SUM(cnt) AS total_rows
FROM (
  SELECT ps1.ps_partkey, COUNT(*) AS cnt
  FROM partsupp AS ps1
  JOIN partsupp AS ps2
    ON ps2.ps_partkey = ps1.ps_partkey
  JOIN partsupp AS ps3
    ON ps3.ps_partkey = ps2.ps_partkey
  JOIN lineitem AS l
    ON l.l_partkey = ps3.ps_partkey
  GROUP BY ps1.ps_partkey
) AS q;
