
--connection slave

SET @saved_slave_type_conversions = @@slave_type_conversions;
SET GLOBAL SLAVE_TYPE_CONVERSIONS = 'ALL_NON_LOSSY';

--connection master
if (!$engine_type)  {let $engine_type=`SELECT @@default_storage_engine`; let $engine_type2=`SELECT @@default_storage_engine`;}

--disable_warnings
DROP TABLE IF EXISTS t1,t2,t3,t4,t5,t6,t7,t8,t9,t10;
--enable_warnings

echo --------------------------------------------------------------------------------------------------------------;
echo --------------------------------------------COMPRESSED TO COMPRESSED------------------------------------------;
echo --------------------------------------------------------------------------------------------------------------;

--sync_slave_with_master
eval CREATE TABLE  t1  (a BLOB COMPRESSED default "1111111111") ENGINE=$engine_type2;
eval CREATE TABLE  t2  (a TINYBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t3  (a MEDIUMBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t4  (a LONGBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t5  (a TEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t6  (a TINYTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t7  (a MEDIUMTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t8  (a LONGTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE  t9  (a VARCHAR(10000) COMPRESSED default "0") ENGINE=$engine_type2;
eval CREATE TABLE  t10 (a VARBINARY(10000) COMPRESSED) ENGINE=$engine_type2;

--connection master
eval CREATE TABLE  t1  (a BLOB COMPRESSED default "1111111111") ENGINE=$engine_type;
eval CREATE TABLE  t2  (a TINYBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t3  (a MEDIUMBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t4  (a LONGBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t5  (a TEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t6  (a TINYTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t7  (a MEDIUMTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t8  (a LONGTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE  t9  (a VARCHAR(10000) COMPRESSED default "0") ENGINE=$engine_type;
eval CREATE TABLE  t10 (a VARBINARY(10000) COMPRESSED) ENGINE=$engine_type;

INSERT INTO t1(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),(default);
INSERT INTO t2(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t3(a)  VALUES(REPEAT('a',100)),(REPEAT('v',2000)),("April"),(NULL);
INSERT INTO t4(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("00-00-00");
INSERT INTO t5(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL);
INSERT INTO t6(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t7(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',3000)),(NULL);
INSERT INTO t8(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("qwerty");
INSERT INTO t9(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("12-12-2009"),(default);
INSERT INTO t10(a) VALUES(REPEAT('a',1000)),(REPEAT('v',2000)),(REPEAT('r',300)),(NULL), ("");

echo --------------------------------------------------data on master------------------------------------------------;

let $a= 10;
while ($a)
{
	--sorted_result
 	eval SELECT LENGTH(a), SUBSTRING(a,1,10), "M,compressed" from t$a;
 	--sorted_result
 	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
 	dec $a;
}
SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');

echo --------------------------------------------------data on slave-------------------------------------------------;

--sync_slave_with_master
let $a= 10;
while ($a)
{
	--sorted_result
	eval SELECT LENGTH(a), SUBSTRING(a,1,10), "S,compressed" from t$a;
	--sorted_result
	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
	dec $a;
}

SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');

--connection master
DROP TABLE t1,t2,t3,t4,t5,t6,t7,t8,t9,t10;
FLUSH STATUS;

echo -----------------------------------------------------------------------------------------------------------------;
echo ---------------------------------------COMPRESSED TO NOT COMPRESSED----------------------------------------------;
echo -----------------------------------------------------------------------------------------------------------------;

--sync_slave_with_master
FLUSH STATUS;
eval CREATE TABLE t1  (a BLOB) ENGINE=$engine_type2;
eval CREATE TABLE t2  (a TINYBLOB) ENGINE=$engine_type2;
eval CREATE TABLE t3  (a MEDIUMBLOB) ENGINE=$engine_type2;
eval CREATE TABLE t4  (a LONGBLOB) ENGINE=$engine_type2;
eval CREATE TABLE t5  (a TEXT) ENGINE=$engine_type2;
eval CREATE TABLE t6  (a TINYTEXT) ENGINE=$engine_type2;
eval CREATE TABLE t7  (a MEDIUMTEXT) ENGINE=$engine_type2;
eval CREATE TABLE t8  (a LONGTEXT) ENGINE=$engine_type2;
eval CREATE TABLE t9  (a VARCHAR(10000)) ENGINE=$engine_type2;
eval CREATE TABLE t10 (a VARBINARY(10000)) ENGINE=$engine_type2;

--connection master
eval CREATE TABLE IF NOT EXISTS t1  (a BLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t2  (a TINYBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t3  (a MEDIUMBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t4  (a LONGBLOB COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t5  (a TEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t6  (a TINYTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t7  (a MEDIUMTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t8  (a LONGTEXT COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t9  (a VARCHAR(10000) COMPRESSED) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t10 (a VARBINARY(10000) COMPRESSED) ENGINE=$engine_type;

INSERT INTO t1(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL);
INSERT INTO t2(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t3(a)  VALUES(REPEAT('a',100)),(REPEAT('v',2000)),("April"),(NULL);
INSERT INTO t4(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("00-00-00");
INSERT INTO t5(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL);
INSERT INTO t6(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t7(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',3000)),(NULL);
INSERT INTO t8(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("qwerty");
INSERT INTO t9(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("12-12-2009");
INSERT INTO t10(a) VALUES(REPEAT('a',1000)),(REPEAT('v',2000)),(REPEAT('r',300)),(NULL), ("");

echo ---------------------------------------data on master-----------------------------------------------------------;

let $a= 10;
while ($a)
{
	--sorted_result
	eval SELECT LENGTH(a),SUBSTRING(a,1,10), "M,compressed"  from t$a;
	--sorted_result
 	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
	dec $a;
}
SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');

echo ---------------------------------------data on slave-------------------------------------------------------------;

--sync_slave_with_master
let $a= 10;
while ($a)
{
	--sorted_result
 	eval SELECT LENGTH(a), SUBSTRING(a,1,10), "S,not compressed" from t$a;
	--sorted_result
	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
	dec $a;
}

SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');
--connection master
DROP TABLE t1,t2,t3,t4,t5,t6,t7,t8,t9,t10;
FLUSH STATUS;

echo -----------------------------------------------------------------------------------------------------------------;
echo -----------------------------------NOT COMPRESSED to COMPRESSED--------------------------------------------------;
echo -----------------------------------------------------------------------------------------------------------------;

--sync_slave_with_master
FLUSH STATUS;
eval CREATE TABLE t1  (a BLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t2  (a TINYBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t3  (a MEDIUMBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t4  (a LONGBLOB COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t5  (a TEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t6  (a TINYTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t7  (a MEDIUMTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t8  (a LONGTEXT COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t9  (a VARCHAR(10000) COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t10 (a VARBINARY(10000) COMPRESSED) ENGINE=$engine_type2;

--connection master
eval CREATE TABLE IF NOT EXISTS t1  (a BLOB) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t2  (a TINYBLOB) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t3  (a MEDIUMBLOB) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t4  (a LONGBLOB) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t5  (a TEXT) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t6  (a TINYTEXT) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t7  (a MEDIUMTEXT) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t8  (a LONGTEXT) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t9  (a VARCHAR(10000)) ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t10 (a VARBINARY(10000)) ENGINE=$engine_type;

INSERT INTO t1(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL);
INSERT INTO t2(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t3(a)  VALUES(REPEAT('a',100)),(REPEAT('v',2000)),("April"),(NULL);
INSERT INTO t4(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("00-00-00");
INSERT INTO t5(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL);
INSERT INTO t6(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',254)),(NULL); 
INSERT INTO t7(a)  VALUES(REPEAT('a',100)),(REPEAT('v',200)),(REPEAT('r',3000)),(NULL);
INSERT INTO t8(a)  VALUES(REPEAT('a',10000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("qwerty");
INSERT INTO t9(a)  VALUES(REPEAT('a',1000)),(REPEAT('v',200)),(REPEAT('r',300)),(NULL),("12-12-2009");
INSERT INTO t10(a) VALUES(REPEAT('a',1000)),(REPEAT('v',2000)),(REPEAT('r',300)),(NULL), ("");

echo -----------------------------------data on master---------------------------------------------------------------;

let $a= 10;
while ($a)
{
	--sorted_result
	eval SELECT LENGTH(a),SUBSTRING(a,1,10), "M,not compressed" from t$a;
	--sorted_result
 	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
	dec $a;
}
SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');

echo -----------------------------------data on slave-----------------------------------------------------------------;

--sync_slave_with_master
let $a= 10;
while ($a)
{
	--sorted_result
	eval SELECT LENGTH(a), SUBSTRING(a,1,10), "S,compressed" from t$a;
	--sorted_result
	eval SELECT DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t$a';
	dec $a;
}
SELECT * FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME IN('Column_compressions', 'Column_decompressions');

--connection master
DROP TABLE t1,t2,t3,t4,t5,t6,t7,t8,t9,t10;

echo -----------------------------------------------------------------------------------------------------------------;
echo ------varchar(1000)->varchar(999) compressed, varchar(1000) compressed->varchar(999) ----------------------------;
echo -----------------------------------------------------------------------------------------------------------------;

--sync_slave_with_master
eval CREATE TABLE t1  (a VARCHAR(999) COMPRESSED) ENGINE=$engine_type2;
eval CREATE TABLE t2  (a VARCHAR(999)) ENGINE=$engine_type2;
show warnings;
call mtr.add_suppression("Slave: Data too long for column 'a' at row 1 Error_code: 1406");
call mtr.add_suppression("Slave: Table 't2' already exists Error_code: 1050");

--connection master
eval CREATE TABLE IF NOT EXISTS t1 (a VARCHAR(1000))ENGINE=$engine_type;
eval CREATE TABLE IF NOT EXISTS t2 (a VARCHAR(1000) COMPRESSED)ENGINE=$engine_type;

INSERT INTO t1(a) VALUES(REPEAT('a',1000)); 
--connection slave
--let $slave_sql_errno= 1406,1677
--source include/wait_for_slave_sql_error_and_skip.inc
show warnings;

--connection master
 INSERT INTO t2(a) VALUES(REPEAT('a',1000)); 
--connection slave
--let $slave_sql_errno= 1406,1677
--source include/wait_for_slave_sql_error_and_skip.inc
show warnings;

SET GLOBAL SLAVE_TYPE_CONVERSIONS = @saved_slave_type_conversions;

--connection master
DROP TABLE t1,t2;
--sync_slave_with_master


