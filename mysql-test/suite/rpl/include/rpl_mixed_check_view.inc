#########################################
# Author: Serge Kozlov skozlov@mysql.com
# Date: 07/10/2006
# Purpose: Showing view from work tables on master and on slave
# Requirements: 
#########################################

SHOW CREATE VIEW v1;
SELECT * FROM v1 ORDER BY a;
sync_slave_with_master;
USE test_rpl;
SHOW CREATE VIEW v1;
SELECT * FROM v1 ORDER BY a;
connection master;

