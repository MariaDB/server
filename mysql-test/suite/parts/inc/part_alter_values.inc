--echo #
--echo # MDEV-14641 Incompatible key or row definition between the MariaDB .frm file and the information in the storage engine
--echo #

--eval CREATE TABLE t1 (i INT) ENGINE=$engine PARTITION BY LIST(i) (PARTITION p0 VALUES IN (1),  PARTITION p1 VALUES IN (2));
ALTER TABLE t1 ROW_FORMAT=COMPRESSED;
ALTER TABLE t1 DROP PARTITION p1;
SELECT * FROM t1;

# Cleanup
DROP TABLE t1;


--echo #
--echo # MDEV-13788 Server crash when issuing bad SQL partition syntax
--echo #

--eval CREATE TABLE t1 (id int, d date) ENGINE=$engine PARTITION BY RANGE COLUMNS(d) (PARTITION p1 VALUES LESS THAN (MAXVALUE))
SHOW CREATE TABLE t1;
--error ER_PARTITION_REQUIRES_VALUES_ERROR
ALTER TABLE t1 REORGANIZE PARTITION p1 INTO
(
  PARTITION p2, /* Notice no values */
  PARTITION p3 VALUES LESS THAN (MAXVALUE)
);
DROP TABLE t1;


--eval CREATE TABLE t1 (id int, d date) ENGINE=$engine PARTITION BY LIST (id) (PARTITION p1 VALUES IN (1,2,3))
SHOW CREATE TABLE t1;
--error ER_PARTITION_REQUIRES_VALUES_ERROR
ALTER TABLE t1 REORGANIZE PARTITION p1 INTO
(
  PARTITION p2, /* Notice no values */
  PARTITION p3 VALUES IN (4,5,6)
);
DROP TABLE t1;
