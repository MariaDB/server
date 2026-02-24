--echo # Start of type_store_assignment_incompatible.inc

--disable_abort_on_error

SET @sql_mode_save= @@sql_mode;

SET @source_type= (SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS
                   WHERE COLUMN_NAME='source'
                   AND TABLE_NAME='t1'
                   AND TABLE_SCHEMA='test');

SET @target_type= (SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS
                   WHERE COLUMN_NAME='target'
                   AND TABLE_NAME='t1'
                   AND TABLE_SCHEMA='test');

# 'IGNORE' -> ' IGNORE'
SET @ignore= CASE WHEN @ignore IS NULL OR @ignore = '' THEN ''
                  WHEN @ignore NOT LIKE ' %' THEN CONCAT(' ',@ignore)
                  ELSE @ignore
             END;

let $source_type= `(SELECT @source_type)`;
let $target_type= `(SELECT @target_type)`;
let $ignore=`(SELECT @ignore)`;

CREATE TABLE t2 LIKE t1;
ALTER TABLE t2 ADD id INT NOT NULL PRIMARY KEY FIRST;
INSERT INTO t2 VALUES (1,DEFAULT,DEFAULT);
SHOW CREATE TABLE t2;

#
# Single row INSERT..VALUES
#

CREATE TABLE t3 LIKE t2;
eval INSERT$ignore INTO t3 VALUES
  (1,
   (SELECT source FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1));
DROP TABLE t3;

#
# Multi-row INSERT..VALUES
#

# INSERT .. VALUES checks assignment compatibility for the first row only.
# Here the first row is compatible, so no error happens.
# The second row is not compatible. It works according to the
# current sql_mode and the table transaction ability, so it can:
# (a) either raise a warning
# (b) or escalate a warning to an error and abort on the current row
# (c) or escalate a warning to an error and rollback
# Here we test (a) and (b).

SET sql_mode='';
CREATE TABLE t3 LIKE t2;
ALTER TABLE t3 ENGINE=MyISAM;
EXECUTE IMMEDIATE
  CONCAT('CREATE VIEW v3 AS SELECT id,',
         IF(@target_type='geometry','AsText(target)','target'), ' AS target,',
         IF(@source_type='geometry','AsText(source)','source'), ' AS source ',
         ' FROM t3');

eval INSERT$ignore INTO t3 VALUES
  (1,
   (SELECT target FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1)),
  (2,
   (SELECT source FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1));
SELECT * FROM v3;
TRUNCATE TABLE t3;

SET sql_mode=STRICT_ALL_TABLES;
eval INSERT$ignore INTO t3 VALUES
  (1,
   (SELECT target FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1)),
  (2,
   (SELECT source FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1));
SELECT * FROM v3;
TRUNCATE TABLE t3;
SET sql_mode=@sql_mode_save;
DROP TABLE t3;
DROP VIEW v3;

#
# INSERT .. SELECT
#

CREATE TABLE t3 LIKE t2;
eval INSERT$ignore INTO t3 SELECT id,source,source FROM t2;

eval INSERT$ignore INTO t3 (id,target,source) SELECT id,source,source FROM t2;

#
# INSERT .. VALUES .. ON DUPLICATE KEY UPDATE target=source
#

eval INSERT$ignore INTO t3 VALUES (1,DEFAULT,DEFAULT) ON DUPLICATE KEY UPDATE target=source;

eval INSERT$ignore INTO t3 (id,target,source) VALUES (1,DEFAULT,DEFAULT) ON DUPLICATE KEY UPDATE target=source;

#
# INSERT .. SELECT .. ON DUPLICATE KEY UPDATE target=source
#

eval INSERT$ignore INTO t3 SELECT 1,DEFAULT(t2.target),DEFAULT(t2.source) FROM t2 ON DUPLICATE KEY UPDATE t3.target=t2.source;

eval INSERT$ignore INTO t3 (id,target,source) SELECT 1,DEFAULT(t2.target),DEFAULT(t2.source) FROM t2 ON DUPLICATE KEY UPDATE t3.target=t2.source;


#
# UPDATE
#
eval UPDATE$ignore t3 SET target=source;


#
# UPDATE, multi-table
#

eval UPDATE$ignore t2, t3 SET t3.target=t2.source WHERE t2.id=t3.id;


#
# ALTER
#

SET @alter=CONCAT('ALTER', @ignore, ' TABLE t3 MODIFY target ', @source_type);
SELECT @alter;
EXECUTE IMMEDIATE @alter;


DROP TABLE t3;
DROP TABLE t2;

#
# MDEV-28963 Incompatible data type assignment through SP vars is not consistent with columns
#

#
# SP local variables
#
DELIMITER $$;
eval CREATE PROCEDURE p1()
BEGIN
  DECLARE src $source_type DEFAULT NULL;
  DECLARE dst $target_type DEFAULT NULL;
  SET dst=src;
END;
$$
DELIMITER ;$$
CALL p1;
DROP PROCEDURE p1;

#
# SP IN parameters
#

--eval CREATE FUNCTION f1(a $target_type) RETURNS INT RETURN NULL;
SELECT f1((SELECT source FROM t1 ORDER BY source LIMIT 1));
DROP FUNCTION f1;

--eval CREATE PROCEDURE p1(a $target_type) BEGIN END;
CALL p1((SELECT source FROM t1 ORDER BY source LIMIT 1));
DROP PROCEDURE p1;

#
# SP OUT parameters
#

DELIMITER $$;
eval CREATE PROCEDURE p1(OUT dst $target_type)
BEGIN
  DECLARE src $source_type DEFAULT NULL;
  SET dst=src;
END;
$$
eval CREATE PROCEDURE p2()
BEGIN
  DECLARE dst $target_type DEFAULT NULL;
  CALL p1(dst);
END;
$$
DELIMITER ;$$
CALL p2();
SHOW WARNINGS;
DROP PROCEDURE p2;
DROP PROCEDURE p1;


#
# SF RETURN
#

DELIMITER $$;
eval CREATE FUNCTION f1() RETURNS $target_type
BEGIN
  DECLARE rc $source_type DEFAULT NULL;
  RETURN rc;
END;
$$
DELIMITER ;$$
SELECT f1();
DROP FUNCTION f1;

#
# Cursor IN parameters
#

DELIMITER $$;
eval CREATE PROCEDURE p1()
BEGIN
  DECLARE src $source_type DEFAULT NULL;
  DECLARE cur1 CURSOR(t $target_type) FOR SELECT * FROM t1 WHERE target=t;
  OPEN cur1(src);
  CLOSE cur1;
END;
$$
DELIMITER ;$$
CALL p1();
DROP PROCEDURE p1;

#
# FETCH
#

CREATE TABLE t2 LIKE t1;
INSERT INTO t2 VALUES ();

DELIMITER $$;
eval CREATE PROCEDURE p1()
BEGIN
  DECLARE dst $target_type DEFAULT NULL;
  DECLARE cur2 CURSOR FOR SELECT source FROM t2 ORDER BY source LIMIT 1;
  OPEN cur2;
  FETCH cur2 INTO dst;
  CLOSE cur2;
END;
$$
DELIMITER ;$$
CALL p1();
DROP PROCEDURE p1;

DROP TABLE t2;

--enable_abort_on_error

--echo # End of type_store_assignment_incompatible.inc
