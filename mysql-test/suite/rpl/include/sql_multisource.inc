# Populate the active connection server with events that come from varying
# domain and server ids.
#
# Parameters:
#   $blob_trx_size : Size of the blob to insert (used for binlog-in-innodb to
#                    force out-of-band binlogging for events)
#

--disable_query_log

if (!$blob_trx_size)
{
  --let $blob_trx_size= 1
}

--connection $con1
# Save old state
let $ORIG_GTID_DOMAIN_ID = `select @@session.gtid_domain_id`;
let $ORIG_SERVER_ID = `select @@session.server_id`;

--connection $con1
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
CREATE TABLE t1 (a int, b longblob) engine=innodb;

SET @@session.server_id= 3;
CREATE TABLE t2 (a int, b longblob) engine=innodb;

--connection server_1
SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 1;
CREATE TABLE t3 (a int, b longblob) engine=innodb;


# Concurrent transactions from different domains
--connection $con1
--send_eval INSERT INTO t2 values (3, repeat('b', $blob_trx_size))

--connection server_1
--send_eval INSERT INTO t3 values (4, repeat('c', $blob_trx_size))

--connection server_1
--reap
--connection $con1
--reap


# Concurrent transactions from the same domain
--connection $con1
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
BEGIN;
--send_eval INSERT INTO t1 values (1, repeat('a1', $blob_trx_size))

--connection server_1
SET @@session.gtid_domain_id= 0;
SET @@session.server_id= 1;
BEGIN;
--send_eval INSERT INTO t1 values (2, repeat('a2', $blob_trx_size))

--connection $con1
--reap
--connection server_1
--reap
--connection $con1
COMMIT;
--connection server_1
COMMIT;


--connection server_1
SET @@session.gtid_domain_id= 1;
SET @@session.server_id= 4;
CREATE TABLE t4 (a int, b longblob) engine=innodb;

SET @@session.gtid_domain_id= 2;
SET @@session.server_id= 1;
CREATE TABLE t5 (a int, b longblob) engine=innodb;
INSERT INTO t5 values (6, 'e');

FLUSH LOGS;

--eval SET @@session.gtid_domain_id= $ORIG_GTID_DOMAIN_ID
--eval SET @@session.server_id= $ORIG_SERVER_ID

--enable_query_log
