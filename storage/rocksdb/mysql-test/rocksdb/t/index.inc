# 
# Basic syntax related to indexes: 
#   unique and non-unique keys,
#   single- and multi-column keys,
#   index option COMMENT.
#   
# See other index* tests for operations 
# which are less likely to be supported
#
# PRIMARY KEY syntax is covered in index_primary test.
# Index types BTREE|HASH -- in index_type_btree|hash tests.
# SPATIAL -- in type_spatial_indexes test.
# FULLTEXT -- in fulltext_search test.
# KEY_BLOCK_SIZE -- in index_key_block_size test.
#
# Usage to call the test from another test:
#
# A calling test may define $index_type, in which case
#   USING clause will be added to the syntax.
# 

################################################
# TODO:
# A part of the test is disabled because unique indexes
# are not currently supported
################################################


let $using_index_type = ;
if ($index_type)
{
  let $using_index_type = USING $index_type;
}


eval CREATE TABLE t1 (a INT,
  b CHAR(8),
  pk INT PRIMARY KEY,
  KEY $using_index_type (a)
) ENGINE=rocksdb;

--replace_column 7 #
SHOW KEYS IN t1;
DROP TABLE t1;

eval CREATE TABLE t1 (a INT,
  b CHAR(8),
  pk INT PRIMARY KEY,
  KEY a_b $using_index_type (a,b) COMMENT 'a_b index'
) ENGINE=rocksdb;

--replace_column 7 #
SHOW KEYS IN t1;
DROP TABLE t1;

eval CREATE TABLE t1 (a INT,
  b CHAR(8),
  pk INT PRIMARY KEY,
  KEY $using_index_type (a),
  KEY $using_index_type (b)
) ENGINE=rocksdb;

--replace_column 7 #
SHOW KEYS IN t1;
DROP TABLE t1;

--disable_parsing

eval CREATE TABLE t1 (a INT,
  b CHAR(8),
  pk INT PRIMARY KEY,
  UNIQUE INDEX $using_index_type (a)
) ENGINE=rocksdb;

--replace_column 7 #
SHOW KEYS IN t1;
INSERT INTO t1 (a,b) VALUES (1,'a'),(2,'b');
--error ER_DUP_ENTRY,ER_DUP_KEY
INSERT INTO t1 (a,b) VALUES (1,'c');

DROP TABLE t1;

--source drop_table_sync.inc

--enable_parsing

#
# ALTER TABLE
#

CREATE TABLE t1 (a INT, b CHAR(8), pk INT AUTO_INCREMENT PRIMARY KEY) ENGINE=rocksdb;
INSERT INTO t1 (a,b) VALUES (100,'z');

eval ALTER TABLE t1 ADD KEY (a) $using_index_type COMMENT 'simple index on a';
--replace_column 7 #
SHOW INDEX FROM t1;
ALTER TABLE t1 DROP KEY a;
DROP TABLE t1;

--disable_parsing

eval CREATE TABLE t1 (a INT,
  b CHAR(8),
  pk INT AUTO_INCREMENT PRIMARY KEY,
  UNIQUE INDEX $using_index_type (a)
) ENGINE=rocksdb;

--replace_column 7 #
SHOW KEYS IN t1;
INSERT INTO t1 (a,b) VALUES (1,'a'),(2,'b');
--error ER_DUP_ENTRY,ER_DUP_KEY
INSERT INTO t1 (a,b) VALUES (1,'c');

ALTER TABLE t1 DROP INDEX a;
INSERT INTO t1 (a,b) VALUES (1,'c');
--error ER_DUP_ENTRY
eval ALTER TABLE t1 ADD UNIQUE INDEX a(a) $using_index_type;
DROP TABLE t1;

--enable_parsing

#
# Test index prefix length limits.
#
set global rocksdb_large_prefix=0;

CREATE TABLE t1 (
  a BLOB(1024),
  KEY (a(767))
) ENGINE=rocksdb;
DROP TABLE t1;

# Should display warning
CREATE TABLE t1 (
  a BLOB(1024),
  KEY (a(768))
) ENGINE=rocksdb;
DROP TABLE t1;

set global rocksdb_large_prefix=1;

CREATE TABLE t1 (
  a BLOB(4096),
  KEY (a(3072))
) ENGINE=rocksdb;
DROP TABLE t1;

# Should display warning
CREATE TABLE t1 (
  a BLOB(4096),
  KEY (a(3073))
) ENGINE=rocksdb;
DROP TABLE t1;

set global rocksdb_large_prefix=DEFAULT;
