# To be used in a test which requires wsrep plugin to be ACTIVE and enabled
# (i.e. wsrep_on=ON). It includes have_wsrep.inc.

--source include/have_innodb.inc

if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES WHERE VARIABLE_NAME = 'wsrep_on' AND VARIABLE_VALUE='ON'`)
{
  --skip Test requires wsrep_on=ON
}
--source include/wait_until_ready.inc

