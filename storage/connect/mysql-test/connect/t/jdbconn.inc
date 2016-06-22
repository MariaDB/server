--source include/not_embedded.inc

--disable_query_log
--error 0,ER_UNKNOWN_ERROR
CREATE TABLE t1 ENGINE=CONNECT TABLE_TYPE=JDBC CATFUNC=drivers;
if (!`SELECT count(*) FROM INFORMATION_SCHEMA.TABLES
  WHERE TABLE_SCHEMA='test' AND TABLE_NAME='t1'
  AND ENGINE='CONNECT'
  AND (CREATE_OPTIONS LIKE "%`table_type`='JDBC'%" OR CREATE_OPTIONS LIKE '%`table_type`=JDBC%')`)
{
  Skip Need Java support;
}
DROP TABLE t1;

# This is specific and explains why this test is disabled.
# You should edit this file to reflect what is the required files location on your machine.
# This is the path to the JVM library (dll or so)
SET GLOBAL connect_jvm_path='C:\\Program Files\\Java\\jdk1.8.0_77\\jre\\bin\\client';

# The complete class path send when creating the Java Virtual Machine is, in that order:
# 1 - The current directory.
# 2 - The paths of the connect_class_path global variable.
# 3 - The paths of the CLASSPATH environment variable.
# These are the paths to the needed classes or jar files. The Apache ones are only for the JdbcApacheInterface wrapper.
SET GLOBAL connect_class_path='E:\\MariaDB-10.1\\Connect\\storage\\connect;E:\\MariaDB-10.1\\Connect\\sql\\data\\postgresql-9.4.1208.jar;E:\\Oracle\\ojdbc6.jar;E:\\Apache\\commons-dbcp2-2.1.1\\commons-dbcp2-2.1.1.jar;E:\\Apache\\commons-pool2-2.4.2\\commons-pool2-2.4.2.jar;E:\\Apache\\commons-logging-1.2\\commons-logging-1.2.jar';

# On my machine, paths to the JDK classes and to the MySQL and MariaDB drivers are defined in the CLASSPATH environment variable
#CREATE FUNCTION envar RETURNS STRING SONAME 'ha_connect.dll';
#SELECT envar('CLASSPATH');

--enable_query_log
