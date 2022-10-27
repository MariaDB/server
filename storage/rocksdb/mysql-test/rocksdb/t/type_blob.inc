#
# BLOB column types
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (
  pk INT AUTO_INCREMENT PRIMARY KEY,
  b BLOB $extra_col_opts,
  b0 BLOB(0) $extra_col_opts,
  b1 BLOB(1) $extra_col_opts,
  b300 BLOB(300) $extra_col_opts,
  bm BLOB(65535) $extra_col_opts,
  b70k BLOB(70000) $extra_col_opts,
  b17m BLOB(17000000) $extra_col_opts,
  t TINYBLOB $extra_col_opts,
  m MEDIUMBLOB $extra_col_opts,
  l LONGBLOB $extra_col_opts
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

# Valid values 
# (cannot get MAX for all columns due to max_allowed_packet limitations)

INSERT INTO t1 (b,b0,b1,b300,bm,b70k,b17m,t,m,l) VALUES
('','','','','','','','','',''),
('a','b','c','d','e','f','g','h','i','j'),
('test1','test2','test3','test4','test5','test6','test7','test8','test9','test10'),
( REPEAT('a',65535), REPEAT('b',65535), REPEAT('c',255), REPEAT('d',65535), REPEAT('e',65535), REPEAT('f',1048576), HEX(REPEAT('g',1048576)), REPEAT('h',255), REPEAT('i',1048576), HEX(REPEAT('j',1048576)) );

--sorted_result
SELECT LENGTH(b), LENGTH(b0), LENGTH(b1), LENGTH(b300), LENGTH(bm), LENGTH(b70k), LENGTH(b17m), LENGTH(t), LENGTH(m), LENGTH(l) FROM t1;

# Invalid values (produce warnings, except for mediumblob and longblob columns for which the values are within limits)

INSERT INTO t1 (b,b0,b1,b300,bm,b70k,b17m,t,m,l) VALUES
( REPEAT('a',65536), REPEAT('b',65536), REPEAT('c',256), REPEAT('d',65536), REPEAT('e',65536), REPEAT('f',1048576), REPEAT('g',1048576), REPEAT('h',256), REPEAT('i',1048576), REPEAT('j',1048576) );

--sorted_result
SELECT LENGTH(b), LENGTH(b0), LENGTH(b1), LENGTH(b300), LENGTH(bm), LENGTH(b70k), LENGTH(b17m), LENGTH(t), LENGTH(m), LENGTH(l) FROM t1;

--error ER_TOO_BIG_DISPLAYWIDTH
ALTER TABLE t1 ADD COLUMN bbb BLOB(4294967296);

DROP TABLE t1;

