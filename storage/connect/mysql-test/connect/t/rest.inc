--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1
ENGINE=CONNECT TABLE_TYPE=JSON FILE_NAME='users.json'
HTTP='http://jsonplaceholder.typicode.com/users';

if (!`SELECT count(*) FROM INFORMATION_SCHEMA.TABLES
     WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t1'
     AND ENGINE='CONNECT'
     AND CREATE_OPTIONS LIKE "%`table_type`='JSON'%"`)
{
  DROP TABLE IF EXISTS t1;
  Skip Need Curl or Casablanca;
}
DROP TABLE t1;
--enable_query_log

