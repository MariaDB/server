#
# parameters:
# $fk_child  - child table name
# $fk_parent - parent table name
#
--connection node_1
SET GLOBAL wsrep_slave_threads=2;

--eval CREATE TABLE `$fk_parent` ( id INT AUTO_INCREMENT  PRIMARY KEY,  j INT) ENGINE=InnoDB

--eval CREATE TABLE `$fk_child` (id INT AUTO_INCREMENT PRIMARY KEY,  parent_id INT, j INT,  FOREIGN KEY (parent_id) REFERENCES `$fk_parent`(id)) ENGINE=InnoDB

--eval INSERT INTO `$fk_parent` VALUES (23,0)

--connection node_2
--let $wait_condition = SELECT COUNT(*) = 1 FROM `$fk_parent`;
--source include/wait_condition.inc

--connection node_1a
SET GLOBAL DEBUG_DBUG='+d,wsrep_ha_write_row';

--connection node_2
--eval INSERT INTO `$fk_child` VALUES (21,23,0),(22,23,0),(23,23,0)

--connection node_1a
SET DEBUG_SYNC='now WAIT_FOR  wsrep_ha_write_row_reached';

--let $wsrep_received_before = `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME = 'wsrep_received'`

--connection node_2
--eval UPDATE `$fk_parent` SET j=2 WHERE id=23

--connection node_1a
--let $wait_condition = SELECT VARIABLE_VALUE = $wsrep_received_before + 1 FROM INFORMATION_SCHEMA.SESSION_STATUS WHERE VARIABLE_NAME = 'wsrep_received'
--source include/wait_condition.inc

SET GLOBAL DEBUG_DBUG='-d,wsrep_ha_write_row';
SET DEBUG_SYNC='now SIGNAL wsrep_ha_write_row_continue';

SET GLOBAL DEBUG_DBUG="RESET";
SET DEBUG_SYNC = 'RESET';

SET GLOBAL wsrep_slave_threads=DEFAULT;

--connection node_2
--eval drop table `$fk_child`
--eval drop table `$fk_parent`
