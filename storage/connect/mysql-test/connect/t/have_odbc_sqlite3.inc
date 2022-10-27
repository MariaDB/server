--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=ODBC CATFUNC=Drivers;
if ($mysql_errno)
{
  Skip No ODBC support;
}
if (!`SELECT count(*) FROM t1 WHERE Description='SQLite3 ODBC Driver'`)
{
  DROP TABLE t1;
  Skip Need SQLite3 ODBC Driver;
}
SHOW CREATE TABLE t1;
DROP TABLE t1;
--enable_query_log
