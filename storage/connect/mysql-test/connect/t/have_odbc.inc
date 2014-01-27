--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=ODBC CATFUNC=Drivers;
if ($mysql_errno)
{
  Skip No ODBC support;
}
DROP TABLE t1;
--enable_query_log
