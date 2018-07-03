#########################################
# Author: Serge Kozlov skozlov@mysql.com
# Date: 07/10/2006
# Purpose: Showing user info on master and on slave
# Requirements: 
#########################################

SELECT host, user, password, plugin, authentication_string, select_priv FROM mysql.user WHERE user LIKE 'user_test_rpl%';
sync_slave_with_master;
USE test_rpl;
SELECT host, user, password, plugin, authentication_string, select_priv FROM mysql.user WHERE user LIKE 'user_test_rpl%';
connection master;

