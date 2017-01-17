--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1 (a CHAR(10)) ENGINE=CONNECT TABLE_TYPE=ZIP FILE_NAME='test.zip';
if ($mysql_errno)
{
  Skip No ZIP support;
}
#if (!`SELECT count(*) FROM INFORMATION_SCHEMA.TABLES
#     WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t1'
#     AND ENGINE='CONNECT'
#     AND CREATE_OPTIONS LIKE '%`table_type`=ZIP%'
#     AND CREATE OPTIONS LIKE "%`file_name`='test.zip'%"`)
#{
#  DROP TABLE IF EXISTS t1;
#  Skip Need ZIP support;
#}
DROP TABLE t1;
--enable_query_log

