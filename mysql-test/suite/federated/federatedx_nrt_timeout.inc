# net_read_timeout of the pushed down multi-table statement $nrt_stmt, which
# the remote server executes as a statement matching $nrt_pattern.
# $nrt_after is run after the timeout, without and with pushdown.

--echo # Hold the table lock on the remote server, so the pushed down statement times out
connection slave;
TRUNCATE TABLE mysql.general_log;
LOCK TABLES federated_nrt.s1 WRITE;
connection master;
--enable_info
set global federated_pushdown=ON;
--error ER_NET_READ_INTERRUPTED
eval $nrt_stmt;
--disable_info

--echo # Kill the abandoned statement on the remote server and release the lock
--source federatedx_nrt_kill.inc
UNLOCK TABLES;

--echo # Run the same kind of statement without and with pushdown after the timeout
connection master;
--enable_info
set global federated_pushdown=OFF;
eval $nrt_after;
set global federated_pushdown=ON;
eval $nrt_after;
--disable_info
set global federated_pushdown= @old_federated_pushdown;

--echo # Repeat the timeout inside a transaction
connection slave;
TRUNCATE TABLE mysql.general_log;
LOCK TABLES federated_nrt.s1 WRITE;
connection master;
set global federated_pushdown=ON;
SET autocommit=0;
--error ER_NET_READ_INTERRUPTED
eval $nrt_stmt;
SELECT @@in_transaction;

--echo # Kill the abandoned statement and release the lock
--source federatedx_nrt_kill.inc
UNLOCK TABLES;

--echo # Statements after the timeout, then end the transaction
connection master;
INSERT INTO federated_nrt.s1 VALUES (0);
ROLLBACK;
SET autocommit=1;
set global federated_pushdown= @old_federated_pushdown;

--echo # Statements the remote server received for the new connection
connection slave;
SELECT argument FROM mysql.general_log
WHERE argument IN ('SET AUTOCOMMIT=0', 'ROLLBACK') OR argument LIKE 'INSERT %'
ORDER BY event_time;
DELETE FROM federated_nrt.s1 WHERE a=0;
SELECT COUNT(*) FROM federated_nrt.s1;
