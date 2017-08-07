--source include/have_rocksdb.inc

# 
# Basic LOAD DATA statements
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

CREATE TABLE t1 (a INT, b CHAR(8), pk INT AUTO_INCREMENT PRIMARY KEY) ENGINE=rocksdb;

let $datadir = `SELECT @@datadir`;

--write_file $datadir/se_loaddata.dat
1,foo,
2,bar,
3,,
4,abc,
EOF

--replace_result $datadir <DATADIR>
eval
LOAD DATA INFILE '$datadir/se_loaddata.dat' INTO TABLE t1 
  FIELDS TERMINATED BY ',' (a,b);
--sorted_result
SELECT a,b FROM t1;

--replace_result $datadir <DATADIR>
eval
LOAD DATA LOCAL INFILE '$datadir/se_loaddata.dat' INTO TABLE t1
  CHARACTER SET utf8 COLUMNS TERMINATED BY ','
  ESCAPED BY '/' (a,b);
--sorted_result
SELECT a,b FROM t1;

--remove_file $datadir/se_loaddata.dat
--write_file $datadir/se_loaddata.dat
5;YYY;
102;'zzz';
0;'test';
EOF

--replace_result $datadir <DATADIR>
eval
LOAD DATA LOCAL INFILE '$datadir/se_loaddata.dat' INTO TABLE t1
  FIELDS TERMINATED BY ';'
  (a) SET b='loaded';

--sorted_result
SELECT a,b FROM t1;

--remove_file $datadir/se_loaddata.dat
--write_file $datadir/se_loaddata.dat
prefix:5;'foo';
prefix:6;'';
prefix:100;foo;
prefix:7;'test';suffix
101;abc;
102;'z';
prefix:0;;
EOF

--replace_result $datadir <DATADIR>
eval
LOAD DATA INFILE '$datadir/se_loaddata.dat' INTO TABLE t1
  FIELDS TERMINATED BY ';'
  OPTIONALLY ENCLOSED BY ''''
  LINES STARTING BY 'prefix:' 
  IGNORE 2 LINES (a,b);

--sorted_result
SELECT a,b FROM t1;

--remove_file $datadir/se_loaddata.dat
--write_file $datadir/se_loaddata.dat
1	foo
2	bar
3	
4	abc
EOF

--replace_result $datadir <DATADIR>
eval
LOAD DATA INFILE '$datadir/se_loaddata.dat' INTO TABLE t1;
--sorted_result
SELECT a,b FROM t1;
--remove_file $datadir/se_loaddata.dat


--write_file $datadir/se_replacedata.dat
1	aaa
2	bbb
3	ccc
4	ddd
EOF

--replace_result $datadir <DATADIR>
if ($skip_unique_check == 1)
{
  --error ER_ON_DUPLICATE_DISABLED
  eval
  LOAD DATA INFILE '$datadir/se_replacedata.dat' REPLACE INTO TABLE t1;
}
if ($skip_unique_check == 0)
{
  eval
  LOAD DATA INFILE '$datadir/se_replacedata.dat' REPLACE INTO TABLE t1;
}
--sorted_result
SELECT a,b FROM t1;
--remove_file $datadir/se_replacedata.dat


# Cleanup
DROP TABLE t1;

