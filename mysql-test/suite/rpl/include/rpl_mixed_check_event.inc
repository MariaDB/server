#########################################
# Author: Serge Kozlov skozlov@mysql.com
# Date: 07/10/2006
# Purpose: Showing events from work tables on master and on slave
# Requirements: 
#########################################

--replace_column 8 # 9 #
SHOW EVENTS;
sync_slave_with_master;
USE test_rpl;
--replace_column 8 # 9 #
SHOW EVENTS;
connection master;

