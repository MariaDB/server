--source include/have_innodb.inc

if (`SELECT COUNT(*) = 0 from INFORMATION_SCHEMA.GLOBAL_VARIABLES
    WHERE VARIABLE_NAME = 'INNODB_DISALLOW_WRITES'`) {
  --skip Test requires 'innodb_disallow_writes'
}
