--echo # Start of type_store_assignment_incompatible.inc

SET @source_type= (SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS
                   WHERE COLUMN_NAME='source'
                   AND TABLE_NAME='t1'
                   AND TABLE_SCHEMA='test');

SET @target_type= (SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS
                   WHERE COLUMN_NAME='target'
                   AND TABLE_NAME='t1'
                   AND TABLE_SCHEMA='test');

CREATE TABLE t2 LIKE t1;
ALTER TABLE t2 ADD id INT NOT NULL PRIMARY KEY FIRST;
INSERT INTO t2 VALUES (1,DEFAULT,DEFAULT);
SHOW CREATE TABLE t2;

#
# Single row INSERT..VALUES
#

CREATE TABLE t3 LIKE t2;
--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 VALUES
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

--error 0,ER_CANT_CREATE_GEOMETRY_OBJECT
INSERT INTO t3 VALUES
  (1,
   (SELECT target FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1)),
  (2,
   (SELECT source FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1));
SELECT * FROM v3;
TRUNCATE TABLE t3;

SET sql_mode=STRICT_ALL_TABLES;
--error ER_TRUNCATED_WRONG_VALUE, ER_TRUNCATED_WRONG_VALUE_FOR_FIELD, WARN_DATA_TRUNCATED, ER_CANT_CREATE_GEOMETRY_OBJECT
INSERT INTO t3 VALUES
  (1,
   (SELECT target FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1)),
  (2,
   (SELECT source FROM t2 ORDER BY id LIMIT 1),
   (SELECT source FROM t2 ORDER BY id LIMIT 1));
SELECT * FROM v3;
TRUNCATE TABLE t3;
SET sql_mode=DEFAULT;
DROP TABLE t3;
DROP VIEW v3;

#
# INSERT .. SELECT
#

CREATE TABLE t3 LIKE t2;
--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 SELECT id,source,source FROM t2;

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 (id,target,source) SELECT id,source,source FROM t2;

#
# INSERT .. VALUES .. ON DUPLICATE KEY UPDATE target=source
#

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 VALUES (1,DEFAULT,DEFAULT) ON DUPLICATE KEY UPDATE target=source;

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 (id,target,source) VALUES (1,DEFAULT,DEFAULT) ON DUPLICATE KEY UPDATE target=source;

#
# INSERT .. SELECT .. ON DUPLICATE KEY UPDATE target=source
#

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 SELECT 1,DEFAULT(t2.target),DEFAULT(t2.source) FROM t2 ON DUPLICATE KEY UPDATE t3.target=t2.source;

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
INSERT INTO t3 (id,target,source) SELECT 1,DEFAULT(t2.target),DEFAULT(t2.source) FROM t2 ON DUPLICATE KEY UPDATE t3.target=t2.source;


#
# UPDATE
#
--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
UPDATE t3 SET target=source;


#
# UPDATE, multi-table
#

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
UPDATE t2, t3 SET t3.target=t2.source WHERE t2.id=t3.id;


#
# ALTER
#

SET @alter=CONCAT('ALTER TABLE t3 MODIFY target ', @source_type);
SELECT @alter;
--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
EXECUTE IMMEDIATE @alter;


DROP TABLE t3;
DROP TABLE t2;
--echo # End of type_store_assignment_incompatible.inc
