set connect_enable_mongo=1;
set connect_json_all_path=0;

--echo #
--echo # Test the MONGO table type
--echo #
eval CREATE TABLE t1 (Document varchar(1024) JPATH='*')
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants $CONN
OPTION_LIST='Driver=$DRV,Version=$VERS' DATA_CHARSET=utf8;
SELECT * from t1 limit 3;
DROP TABLE t1;

--echo #
--echo # Test catfunc
--echo #
eval CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants CATFUNC=columns
OPTION_LIST='Depth=1,Driver=$DRV,Version=$VERS' DATA_CHARSET=utf8 $CONN;
SELECT * from t1;
DROP TABLE t1;

--echo #
--echo # Explicit columns
--echo #
eval CREATE TABLE t1 (
_id VARCHAR(24) NOT NULL,
name VARCHAR(255) NOT NULL,
cuisine VARCHAR(255) NOT NULL,
borough VARCHAR(255) NOT NULL,
restaurant_id VARCHAR(255) NOT NULL)
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants
CONNECTION='mongodb://localhost:27017' LRECL=1024 DATA_CHARSET=utf8
OPTION_LIST='Driver=$DRV,Version=$VERS';
SELECT * FROM t1 LIMIT 10;
DROP TABLE t1;

--echo #
--echo # Test discovery
--echo #
eval CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants
OPTION_LIST='Depth=1,Driver=$DRV,Version=$VERS' $CONN DATA_CHARSET=utf8;
SHOW CREATE TABLE t1;
SELECT * FROM t1 LIMIT 5;
DROP TABLE t1;

--echo #
--echo # Dropping a column
--echo #
let $COLIST= $PROJ{"grades":0}$ENDP;
eval CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants DATA_CHARSET=utf8
COLIST='$COLIST' OPTION_LIST='Driver=$DRV,Version=$VERS,level=0' $CONN;
SELECT * FROM t1 LIMIT 10;
DROP TABLE t1;

--echo #
--echo # Specifying Jpath
--echo #
eval CREATE TABLE t1 (
_id VARCHAR(24) NOT NULL,
name VARCHAR(64) NOT NULL,
cuisine CHAR(200) NOT NULL,
borough CHAR(16) NOT NULL,
street VARCHAR(65) JPATH='address.street',
building CHAR(16) JPATH='address.building',
zipcode CHAR(5) JPATH='address.zipcode',
grade CHAR(1) JPATH='grades.0.grade',
score INT(4) NOT NULL JPATH='grades.0.score',
`date` DATE JPATH='grades.0.date',
restaurant_id VARCHAR(255) NOT NULL)
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='restaurants' DATA_CHARSET=utf8
OPTION_LIST='Driver=$DRV,Version=$VERS' $CONN;
--vertical_results
SELECT * FROM t1 LIMIT 1;
--horizontal_results
SELECT name, street, score, date FROM t1 LIMIT 5;
SELECT name, cuisine, borough FROM t1 WHERE grade = 'A' LIMIT 10;
SELECT COUNT(*) FROM t1 WHERE grade = 'A';
SELECT * FROM t1 WHERE cuisine = 'English';
SELECT * FROM t1 WHERE score = building;
DROP TABLE t1;

--echo #
--echo # Specifying Filter
--echo #
eval CREATE TABLE t1 (
_id CHAR(24) NOT NULL,
name CHAR(64) NOT NULL,
borough CHAR(16) NOT NULL,
restaurant_id CHAR(8) NOT NULL)
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants DATA_CHARSET=utf8
FILTER='{"cuisine":"French","borough":{"\$ne":"Manhattan"}}'
OPTION_LIST='Driver=$DRV,Version=$VERS' $CONN;
SELECT name FROM t1 WHERE borough = 'Queens';
DROP TABLE t1;

--echo #
--echo # Testing pipeline
--echo #
eval CREATE TABLE t1 (
name VARCHAR(64) NOT NULL,
borough CHAR(16) NOT NULL,
date DATETIME NOT NULL,
grade CHAR(1) NOT NULL,
score INT(4) NOT NULL)
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='restaurants' DATA_CHARSET=utf8
COLIST='{"pipeline":[{"\$match":{"cuisine":"French"}},{"\$unwind":"\$grades"},{"\$project":{"_id":0,"name":1,"borough":1,"date":"\$grades.date","grade":"\$grades.grade","score":"\$grades.score"}}]}'
OPTION_LIST='Driver=$DRV,Version=$VERS,Pipeline=1' $CONN;
SELECT * FROM t1 LIMIT 10;
SELECT name, grade, score, date FROM t1 WHERE borough = 'Bronx';
DROP TABLE t1;

--echo #
--echo # try level 2 discovery
--echo #
let $COLIST= $PROJ{"cuisine":0}$ENDP;
eval CREATE TABLE t1
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME=restaurants
FILTER='{"cuisine":"French","borough":{"\$ne":"Manhattan"}}'
COLIST='$COLIST' $CONN
OPTION_LIST='Driver=$DRV,level=2,version=$VERS';
SHOW CREATE TABLE t1;
IF ($TYPE == MONGO)
{
SELECT name, borough, address_street, grades_0_score AS score FROM t1 WHERE grades_0_grade = 'B';
}
IF ($TYPE == JSON)
{
SELECT name, borough, address_street, grades_score AS score FROM t1 WHERE grades_grade = 'B';
}
IF ($TYPE == BSON)
{
SELECT name, borough, address_street, grades_score AS score FROM t1 WHERE grades_grade = 'B';
}
DROP TABLE t1;

--echo #
--echo # try CRUD operations
--echo #
--disable_query_log
--exec $MONGO --eval "db.testcoll.drop()" --quiet
--enable_query_log
eval CREATE TABLE t1 (_id INT(4) NOT NULL, msg CHAR(64))
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='testcoll'
OPTION_LIST='Driver=$DRV,Version=$VERS' $CONN;
DELETE FROM t1;
INSERT INTO t1 VALUES(0,NULL),(1,'One'),(2,'Two'),(3,'Three');
SELECT * FROM t1;
UPDATE t1 SET msg = 'Deux' WHERE _id = 2;
DELETE FROM t1 WHERE msg IS NULL;
SELECT * FROM t1;
DELETE FROM t1;
DROP TABLE t1;
--exec $MONGO --eval "db.testcoll.drop()" --quiet

--echo #
--echo # List states whose population is equal or more than 10 millions
--echo #
--disable_query_log
--exec $MONGO --eval "db.cities.drop()" --quiet
--enable_query_log
--exec $MONGOIMPORT --quiet $MTR_SUITE_DIR/std_data/cities.json
eval CREATE TABLE t1 (
  _id char(5) NOT NULL,
  city char(16) NOT NULL,
  loc_0 double(12,6) NOT NULL `JPATH`='loc.0',
  loc_1 char(12) NOT NULL `JPATH`='loc.1',
  pop int(11) NOT NULL,
  state char(2) NOT NULL)
ENGINE=CONNECT CONNECTION='mongodb://localhost:27017' TABLE_TYPE=$TYPE TABNAME='cities'
OPTION_LIST='Driver=$DRV,Version=$VERS' $CONN DATA_CHARSET='utf8';
--echo # Using SQL for grouping
SELECT state, sum(pop) AS totalPop FROM t1 GROUP BY state HAVING totalPop >= 10000000 ORDER BY totalPop DESC;
DROP TABLE t1;

--echo # Using a pipeline for grouping
eval CREATE TABLE t1 (_id CHAR(2) NOT NULL, totalPop INT(11) NOT NULL)
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='cities' DATA_CHARSET=utf8
COLIST='{"pipeline":[{"\$group":{"_id":"\$state","totalPop":{"\$sum":"\$pop"}}},{"\$match":{"totalPop":{"\$gte":10000000}}},{"\$sort":{"totalPop":-1}}]}'
OPTION_LIST='Driver=$DRV,Version=$VERS,Pipeline=1' $CONN;
SELECT * FROM t1;
DROP TABLE t1;
--exec $MONGO --eval "db.cities.drop()" --quiet

--echo #
--echo # Test making array
--echo #
eval CREATE TABLE t1 (
  _id int(4) NOT NULL,
  item CHAR(8) NOT NULL,
  prices_0 INT(6) JPATH='prices.0',
  prices_1 INT(6) JPATH='prices.1',
  prices_2 INT(6) JPATH='prices.2',
  prices_3 INT(6) JPATH='prices.3',
  prices_4 INT(6) JPATH='prices.4')
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='testcoll' DATA_CHARSET=utf8
OPTION_LIST='Driver=$DRV,Version=$VERS' $CONN;
INSERT INTO t1 VALUES
(1,'journal',87,45,63,12,78),
(2,'notebook',123,456,789,NULL,NULL),
(3,'paper',5,7,3,8,NULL),
(4,'planner',25,71,NULL,44,27),
(5,'postcard',5,7,3,8,NULL);
SELECT * FROM t1;
DROP TABLE t1;

--echo #
--echo # Test array aggregation
--echo #
eval CREATE TABLE t1
ENGINE=CONNECT TABLE_TYPE=$TYPE TABNAME='testcoll'
COLIST='{"pipeline":[{"\$project":{"_id":0,"item":1,"total":{"\$sum":"\$prices"},"average":{"\$avg":"\$prices"}}}]}'
OPTION_LIST='Driver=$DRV,Version=$VERS,Pipeline=YES' $CONN;
SELECT * FROM t1;
DROP TABLE t1;
--exec $MONGO --eval "db.testcoll.drop()" --quiet

set connect_enable_mongo=0;
