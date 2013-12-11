--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=ODBC CATFUNC=Sources;
if ($mysql_errno)
{
  Skip No ODBC support;
}
if (!`SELECT count(*) FROM t1 WHERE Name='ConnectEnginePostgresql'`)
{
  DROP TABLE t1;
  Skip Need ODBC data source ConnectEnginePostgresql;
}
SHOW CREATE TABLE t1;
DROP TABLE t1;
--enable_query_log
