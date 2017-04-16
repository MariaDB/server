#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption suite).
# Please check all dependent tests after modifying it
#

--source include/have_binlog_format_row.inc
--source include/master-slave.inc

connection slave;
set @saved_slave_type_conversions = @@global.slave_type_conversions;
CREATE TABLE type_conversions (
       TestNo INT AUTO_INCREMENT PRIMARY KEY,
       Source TEXT,
       Target TEXT,
       Flags TEXT,
       On_Master TEXT,
       On_Slave TEXT,
       Expected TEXT,
       Compare INT,
       Error TEXT);

SELECT @@global.slave_type_conversions;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='';
SELECT @@global.slave_type_conversions;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_NON_LOSSY';
SELECT @@global.slave_type_conversions;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_LOSSY';
SELECT @@global.slave_type_conversions;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_LOSSY,ALL_NON_LOSSY';
SELECT @@global.slave_type_conversions;
--error ER_WRONG_VALUE_FOR_VAR
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_LOSSY,ALL_NON_LOSSY,NONEXISTING_BIT';
SELECT @@global.slave_type_conversions;

# Checking strict interpretation of type conversions
connection slave;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='';
source extra/rpl_tests/type_conversions.test;

# Checking lossy integer type conversions
connection slave;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_NON_LOSSY';
source extra/rpl_tests/type_conversions.test;

# Checking non-lossy integer type conversions
connection slave;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_LOSSY';
source extra/rpl_tests/type_conversions.test;

# Checking all type conversions
connection slave;
SET GLOBAL SLAVE_TYPE_CONVERSIONS='ALL_LOSSY,ALL_NON_LOSSY';
source extra/rpl_tests/type_conversions.test;

connection slave;
--echo **** Result of conversions ****
disable_query_log;
SELECT RPAD(Source, 15, ' ') AS Source_Type,
       RPAD(Target, 15, ' ') AS Target_Type,
       RPAD(Flags, 25, ' ') AS All_Type_Conversion_Flags,
       IF(Compare IS NULL AND Error IS NOT NULL, '<Correct error>',
          IF(Compare, '<Correct value>',
             CONCAT("'", On_Slave, "' != '", Expected, "'")))
         AS Value_On_Slave
  FROM type_conversions;
enable_query_log;
DROP TABLE type_conversions;

call mtr.add_suppression("Slave SQL.*Column 1 of table .test.t1. cannot be converted from type.* error.* 1677");

connection master;
DROP TABLE t1;
sync_slave_with_master;

set global slave_type_conversions = @saved_slave_type_conversions;

--source include/rpl_end.inc
