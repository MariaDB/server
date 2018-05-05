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

# You cand edit this file to reflect what is the required files location on your machine.
# This is the path to the JVM library (dll or so)
# If not set CONNECT will try to use the JAVA_HOME environment variable
# and if not found try to find it in the registers (Windows only)
#SET GLOBAL connect_jvm_path='C:\\Program Files\\Java\\jdk1.8.0_77\\jre\\bin\\client';

# The complete class path send when creating the Java Virtual Machine is, in that order:
# 1 - The current directory.
# 2 - The paths of the connect_class_path global variable.
# 3 - The paths of the CLASSPATH environment variable.
# In this test we use an executable jar file that contains all the eisting wrappers.
#eval SET GLOBAL connect_class_path='$MTR_SUITE_DIR/std_data/JdbcMariaDB.jar';
eval SET GLOBAL connect_class_path='$MTR_SUITE_DIR/std_data/JavaWrappers.jar';

# Paths to the JDK classes and to the JDBC drivers should be defined in the CLASSPATH environment variable
#CREATE FUNCTION envar RETURNS STRING SONAME 'ha_connect.dll';
#SELECT envar('CLASSPATH');

--enable_query_log
