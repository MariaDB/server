# include/query_cache.inc
#
# The variables
#     $engine_type       -- storage engine to be tested
#     $test_foreign_keys -- 0, skip foreign key tests
#                        -- 1, do not skip foreign key tests
#     $partitions_a      -- partition by column 'a'
#     $partitions_id     -- partition by column 'id'
#     $partitions_s1     -- partition by column 's1'
# have to be set before sourcing this script.
#
# Last update:
# 2006-08-02 ML test refactored
#               old name was innodb_cache.test
#               main code went into include/query_cache.inc
#

SET global query_cache_type=ON;
SET local query_cache_type=ON;
eval SET SESSION STORAGE_ENGINE = $engine_type;

# Initialise
--disable_warnings
drop table if exists t1,t2,t3;
--enable_warnings

set @save_query_cache_size = @@global.query_cache_size;
set GLOBAL query_cache_size = 1355776;

#
# Without auto_commit.
#
flush status;
set autocommit=0;
eval create table t1 (a int not null)$partitions_a;
insert into t1 values (1),(2),(3);
--sorted_result
select * from t1;
show status like "Qcache_queries_in_cache";
drop table t1;
commit;
set autocommit=1;
begin;
eval create table t1 (a int not null)$partitions_a;
insert into t1 values (1),(2),(3);
--sorted_result
select * from t1;
show status like "Qcache_queries_in_cache";
drop table t1;
commit;
eval create table t1 (a int not null)$partitions_a;
eval create table t2 (a int not null)$partitions_a;
eval create table t3 (a int not null)$partitions_a;
insert into t1 values (1),(2);
insert into t2 values (1),(2);
insert into t3 values (1),(2);
--sorted_result
select * from t1;
--sorted_result
select * from t2;
--sorted_result
select * from t3;
show status like "Qcache_queries_in_cache";
show status like "Qcache_hits";
begin;
--sorted_result
select * from t1;
--sorted_result
select * from t2;
--sorted_result
select * from t3;
show status like "Qcache_queries_in_cache";
show status like "Qcache_hits";
insert into t1 values (3);
insert into t2 values (3);
insert into t1 values (4);
--sorted_result
select * from t1;
--sorted_result
select * from t2;
--sorted_result
select * from t3;
show status like "Qcache_queries_in_cache";
show status like "Qcache_hits";
commit;
show status like "Qcache_queries_in_cache";
drop table t3,t2,t1;

eval CREATE TABLE t1 (id int(11) NOT NULL auto_increment, PRIMARY KEY (id))$partitions_id;
select count(*) from t1;
insert into t1 (id) values (0);
select count(*) from t1;
drop table t1;

if ($test_foreign_keys)
{
#
# one statement roll back inside transation
#
CREATE TABLE t1 ( id int(10) NOT NULL auto_increment, a varchar(25) default NULL, PRIMARY KEY  (id), UNIQUE KEY a (a));
CREATE TABLE t2 ( id int(10) NOT NULL auto_increment, b varchar(25) default NULL, PRIMARY KEY  (id), UNIQUE KEY b (b));
CREATE TABLE t3 ( id int(10) NOT NULL auto_increment, t1_id int(10) NOT NULL default '0', t2_id int(10) NOT NULL default '0', state int(11) default NULL, PRIMARY KEY  (id), UNIQUE KEY t1_id (t1_id,t2_id), KEY t2_id (t2_id,t1_id), CONSTRAINT `t3_ibfk_1` FOREIGN KEY (`t1_id`) REFERENCES `t1` (`id`), CONSTRAINT `t3_ibfk_2` FOREIGN KEY (`t2_id`) REFERENCES `t2` (`id`));
INSERT INTO t1 VALUES (1,'me');
INSERT INTO t2 VALUES (1,'you');
INSERT INTO t3 VALUES (2,1,1,2);
delete from t3 where t1_id = 1 and t2_id = 1;
select t1.* from t1, t2, t3 where t3.state & 1 = 0 and t3.t1_id = t1.id and t3.t2_id = t2.id and t1.id = 1 order by t1.a asc;
begin;
insert into t3 VALUES ( NULL, 1, 1, 2 );
-- error ER_DUP_ENTRY
insert into t3 VALUES ( NULL, 1, 1, 2 );
commit;
select t1.* from t1, t2, t3 where t3.state & 1 = 0 and t3.t1_id = t1.id and t3.t2_id = t2.id and t1.id = 1 order by t1.a asc;
drop table t3,t2,t1;
}

#
# Test query cache with two interleaving transactions
#

# Establish connection1
connect (connection1,localhost,root,,);
eval SET SESSION STORAGE_ENGINE = $engine_type;
SET @@autocommit=1;

connection default;
# This should be 'YES'.
SHOW VARIABLES LIKE 'have_query_cache';

SET GLOBAL query_cache_size = 204800;
flush status;
SET @@autocommit=1;
eval SET SESSION STORAGE_ENGINE = $engine_type;
eval CREATE TABLE t2 (s1 int, s2 varchar(1000), key(s1))$partitions_s1;
INSERT INTO t2 VALUES (1,repeat('a',10)),(2,repeat('a',10)),(3,repeat('a',10)),(4,repeat('a',10));
COMMIT;
START TRANSACTION;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
UPDATE t2 SET s2 = 'w' WHERE s1 = 3;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
show status like "Qcache_queries_in_cache";

connection connection1;
START TRANSACTION;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
INSERT INTO t2 VALUES (5,'w');
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
COMMIT;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';

show status like "Qcache_queries_in_cache";

connection default;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
COMMIT;

show status like "Qcache_queries_in_cache";

SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
show status like "Qcache_queries_in_cache";

connection connection1;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';

START TRANSACTION;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
INSERT INTO t2 VALUES (6,'w');
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';

connection default;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
START TRANSACTION;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
DELETE from t2 WHERE s1=3;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';
COMMIT;

connection connection1;

COMMIT;
SELECT sql_cache count(*) FROM t2 WHERE s2 = 'w';

show status like "Qcache_queries_in_cache";
show status like "Qcache_hits";

# Final cleanup
disconnect connection1;
--source include/wait_until_disconnected.inc
connection default;
set @@global.query_cache_size = @save_query_cache_size;
drop table t2;

SET global query_cache_type=default;
