# "mtr --ps" returns different values in "Max length"
--disable_ps_protocol
--enable_metadata
--vertical_results
SELECT
  a                                         AS ___________a,
  CASE WHEN a IS NOT NULL THEN a END        AS case_______a,
  CASE WHEN a IS NOT NULL THEN a ELSE a END AS case_____a_a,
  COALESCE(a)                               AS coalesce___a,
  COALESCE(a, a)                            AS coalesce_a_a,
  IF(a IS NULL, a, a)                       AS if_______a_a,
  IFNULL(a, a)                              AS ifnull___a_a,
  LEAST(a, a)                               AS least____a_a,
  GREATEST(a, a)                            AS greatest_a_a,
  b                                         AS ___________b,
  CASE WHEN a IS NOT NULL THEN b END        AS case_______b,
  CASE WHEN a IS NOT NULL THEN b ELSE b END AS case_____b_b,
  COALESCE(b)                               AS coalesce___b,
  COALESCE(b, b)                            AS coalesce_b_b,
  IF(a IS NULL, b, b)                       AS if_______b_b,
  IFNULL(b, b)                              AS ifnull___b_b,
  LEAST(b, b)                               AS least____b_b,
  GREATEST(b, b)                            AS greatest_b_b
FROM t1;
SELECT
  CASE WHEN a IS NOT NULL THEN a ELSE b END AS case_____a_b,
  CASE WHEN a IS NOT NULL THEN b ELSE a END AS case_____b_a,
  COALESCE(a, b)                            AS coalesce_a_b,
  COALESCE(b, a)                            AS coalesce_b_a,
  IF(a IS NULL, a, b)                       AS if_______a_b,
  IF(a IS NULL, b, a)                       AS if_______b_a,
  IFNULL(a, b)                              AS ifnull___a_b,
  IFNULL(b, a)                              AS ifnull___b_a,
  LEAST(a, b)                               AS least____a_b,
  LEAST(b, a)                               AS least____b_a,
  GREATEST(a, b)                            AS greatest_a_b,
  GREATEST(b, a)                            AS greatest_b_a
FROM t1;
--horizontal_results
--disable_metadata
--enable_ps_protocol
CREATE TABLE t2 AS
SELECT
  a                                         AS ___________a,
  CASE WHEN a IS NOT NULL THEN a END        AS case_______a,
  CASE WHEN a IS NOT NULL THEN a ELSE a END AS case_____a_a,
  COALESCE(a)                               AS coalesce___a,
  COALESCE(a, a)                            AS coalesce_a_a,
  IF(a IS NULL, a, a)                       AS if_______a_a,
  IFNULL(a, a)                              AS ifnull___a_a,
  LEAST(a, a)                               AS least____a_a,
  GREATEST(a, a)                            AS greatest_a_a,
  b                                         AS ___________b,
  CASE WHEN a IS NOT NULL THEN b END        AS case_______b,
  CASE WHEN a IS NOT NULL THEN b ELSE b END AS case_____b_b,
  COALESCE(b)                               AS coalesce___b,
  COALESCE(b, b)                            AS coalesce_b_b,
  IF(a IS NULL, b, b)                       AS if_______b_b,
  IFNULL(b, b)                              AS ifnull___b_b,
  LEAST(b, b)                               AS least____b_b,
  GREATEST(b, b)                            AS greatest_b_b
FROM t1;
SHOW CREATE TABLE t2;
DROP TABLE t2;
SET STATEMENT sql_mode = 'NO_ENGINE_SUBSTITUTION' FOR
CREATE TABLE t2 AS
SELECT
  CASE WHEN a IS NOT NULL THEN a ELSE b END AS case_____a_b,
  CASE WHEN a IS NOT NULL THEN b ELSE a END AS case_____b_a,
  COALESCE(a, b)                            AS coalesce_a_b,
  COALESCE(b, a)                            AS coalesce_b_a,
  IF(a IS NULL, a, b)                       AS if_______a_b,
  IF(a IS NULL, b, a)                       AS if_______b_a,
  IFNULL(a, b)                              AS ifnull___a_b,
  IFNULL(b, a)                              AS ifnull___b_a,
  LEAST(a, b)                               AS least____a_b,
  LEAST(b, a)                               AS least____b_a,
  GREATEST(a, b)                            AS greatest_a_b,
  GREATEST(b, a)                            AS greatest_b_a
FROM t1;
SHOW CREATE TABLE t2;
DROP TABLE t2;
