# SQL file with out of order GTIDs coming from various domains and servers

--disable_query_log

# Save old state
let $ORIG_GTID_DOMAIN_ID = `select @@session.gtid_domain_id`;
let $ORIG_SERVER_ID = `select @@session.server_id`;

SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int);
INSERT INTO t1 values (1);

SET @@session.server_id= 3;
CREATE TABLE t2 (a int);
SET @@session.gtid_seq_no= 6;
INSERT INTO t2 values (2);
SET @@session.gtid_seq_no= 5;
INSERT INTO t2 values (1);
SET @@session.gtid_seq_no= 7;
INSERT INTO t2 values (3);

SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 1;
CREATE TABLE t3 (a int);
INSERT INTO t3 values (1);
SET @@session.gtid_seq_no= 4;
INSERT INTO t3 values (3);
SET @@session.gtid_seq_no= 3;
INSERT INTO t3 values (2);
SET @@session.gtid_seq_no= 5;
INSERT INTO t3 values (4);

SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
INSERT INTO t1 values (2);

FLUSH LOGS;

--eval SET @@session.gtid_domain_id= $ORIG_GTID_DOMAIN_ID
--eval SET @@session.server_id= $ORIG_SERVER_ID

--enable_query_log
