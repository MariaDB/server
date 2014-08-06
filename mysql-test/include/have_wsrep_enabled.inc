# To be used in a test which requires wsrep plugin to be ACTIVE and enabled
# (i.e. wsrep_on=ON). It includes have_wsrep.inc.

--source include/have_wsrep.inc

--require r/have_wsrep.require
disable_query_log;
SHOW VARIABLES LIKE 'wsrep_on';
enable_query_log;
