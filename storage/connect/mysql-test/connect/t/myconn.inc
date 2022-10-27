--source include/not_embedded.inc

let $PORT= `select @@port`;

--disable_query_log
--replace_result $PORT PORT
--error 0,ER_UNKNOWN_ERROR
eval CREATE TABLE t1 (a INT) ENGINE=CONNECT TABLE_TYPE=MYSQL
  CONNECTION='mysql://root@127.0.0.1:$SLAVE_MYPORT/test/tx1';
if (!`SELECT count(*) FROM INFORMATION_SCHEMA.TABLES
  WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t1'
  AND ENGINE='CONNECT'
  AND CREATE_OPTIONS LIKE '%`table_type`=MySQL%'`)
{
  Skip Need MySQL support;
}
DROP TABLE t1;
--enable_query_log

connect (master,127.0.0.1,root,,test,$MASTER_MYPORT,);
connect (slave,127.0.0.1,root,,test,$SLAVE_MYPORT,);

connection master;
CREATE DATABASE connect;

connection slave;
CREATE DATABASE connect;
