# RENAME TABLE, ALTER TABLE ... ALGORITHM=COPY, REPAIR TABLE and ADD FULLTEXT
# on an Aria table with a fulltext parser plugin, each on its own table,
# with $row_format and $transactional.
--echo # Fulltext parser with ROW_FORMAT=$row_format
--let $create= CREATE TABLE t1 (a TEXT, FULLTEXT(a) WITH PARSER simple_parser) ENGINE=Aria ROW_FORMAT=$row_format TRANSACTIONAL=$transactional
--let $rows= INSERT INTO t1 VALUES ('abc-def'), ('abcd-efgh'), ('other text')

eval $create;
eval $rows;
RENAME TABLE t1 TO t2;
SELECT a FROM t2 WHERE MATCH(a) AGAINST('abc-def abcd');
DROP TABLE t2;

eval $create;
eval $rows;
ALTER TABLE t1 ADD COLUMN b INT, ALGORITHM=COPY;
SELECT a FROM t1 WHERE MATCH(a) AGAINST('abc-def abcd');
DROP TABLE t1;

# Default repair sorts keys; EXTENDED repairs through the key cache.
eval $create;
eval $rows;
REPAIR TABLE t1;
SELECT a FROM t1 WHERE MATCH(a) AGAINST('abc-def abcd');
DROP TABLE t1;

eval $create;
eval $rows;
REPAIR TABLE t1 EXTENDED;
SELECT a FROM t1 WHERE MATCH(a) AGAINST('abc-def abcd');
DROP TABLE t1;

eval CREATE TABLE t1 (a TEXT)
  ENGINE=Aria ROW_FORMAT=$row_format TRANSACTIONAL=$transactional;
eval $rows;
ALTER TABLE t1 ADD FULLTEXT(a) WITH PARSER simple_parser, ALGORITHM=COPY;
SELECT a FROM t1 WHERE MATCH(a) AGAINST('abc-def abcd');
DROP TABLE t1;
