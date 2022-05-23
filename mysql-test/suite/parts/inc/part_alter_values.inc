--source include/have_symlink.inc

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

--echo #
--echo # MDEV-15456 Server crashes upon adding or dropping a partition in ALTER under LOCK TABLE after ER_SAME_NAME_PARTITION
--echo #
--eval create table t1 (i int) engine=$engine partition by range(i) (partition p0 values less than (10))
lock table t1 write;
--error ER_SAME_NAME_PARTITION
alter table t1 add partition (partition p0 values less than (20));
alter table t1 add partition (partition p1 values less than (20)) /* comment */;
drop table t1;

--echo #
--echo # MDEV-27065 Partitioning tables with custom data directories moves data back to default directory
--echo #

--mkdir $MYSQLTEST_VARDIR/tmp/mdev_27065

--disable_query_log
--eval CREATE TABLE t1 (id INT) ENGINE=$engine DATA DIRECTORY='$MYSQLTEST_VARDIR/tmp/mdev_27065'
--enable_query_log
ALTER TABLE t1 PARTITION BY RANGE(id)(
  PARTITION p0 VALUES LESS THAN (1000),
  PARTITION p1 VALUES LESS THAN MAXVALUE
);
DROP TABLE t1;

# InnoDB doesn't support INDEX DIRECTORY.
if (`SELECT IF('$engine' != 'InnoDB', 1, 0)`)
{
  --disable_query_log
  --eval CREATE TABLE t2 (id INT) ENGINE=$engine INDEX DIRECTORY='$MYSQLTEST_VARDIR/tmp/mdev_27065'
  --enable_query_log
  ALTER TABLE t2 PARTITION BY RANGE(id)(
    PARTITION p0 VALUES LESS THAN (1000),
    PARTITION p1 VALUES LESS THAN MAXVALUE
  );
  DROP TABLE t2;
}

--remove_files_wildcard $MYSQLTEST_VARDIR/tmp/mdev_27065 *
--rmdir $MYSQLTEST_VARDIR/tmp/mdev_27065
