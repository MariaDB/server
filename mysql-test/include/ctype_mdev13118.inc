--echo #
--echo # MDEV-13118 Wrong results with LOWER and UPPER and subquery
--echo #

SET @save_optimizer_switch=@@optimizer_switch;
SET optimizer_switch=_latin1'derived_merge=on';
CREATE TABLE t1 AS SELECT REPEAT('a', 10) AS t LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('abcdefghi'),('ABCDEFGHI');
--sorted_result
SELECT CONCAT(t2,'-',t2) c2 FROM (SELECT LOWER(t) t2 FROM t1) sub;
--sorted_result
SELECT CONCAT(t2,'-',t2) c2 FROM (SELECT UPPER(t) t2 FROM t1) sub;
DROP TABLE t1;
SET optimizer_switch=@save_optimizer_switch;
