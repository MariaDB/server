#
# Initialization for MW-328 tests
#

CREATE TABLE t1 (f1 INTEGER AUTO_INCREMENT PRIMARY KEY, f2 CHAR(20) DEFAULT 'abc') ENGINE=InnoDB;
INSERT INTO t1 (f1) VALUES (1);

CREATE TABLE t2 (f1 CHAR(20)) ENGINE=InnoDB;

#
# Have some random updates going on against t1
#

DELIMITER |;
CREATE PROCEDURE proc_update ()
BEGIN
        DECLARE CONTINUE HANDLER FOR SQLEXCEPTION BEGIN END;
        SET SESSION wsrep_sync_wait = 0;
        WHILE 1 DO
                UPDATE t1 SET f2 = LEFT(MD5(RAND()), 4);
        END WHILE;
END|

DELIMITER ;|

--connect node_1X, 127.0.0.1, root, , test, $NODE_MYPORT_1
--connection node_1X
--let $sp_connection_id = `SELECT CONNECTION_ID()`
--send CALL proc_update();
