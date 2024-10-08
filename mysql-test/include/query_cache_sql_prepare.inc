############### include/query_cache_sql_prepare.inc ################
#
# This is to see how statements prepared via the PREPARE SQL command
# go into the query cache.
# Query cache is abbreviated as "QC"
#
# Last update:
# 2008-05-26 Kostja
#               - Add test coverage for automatic statement reprepare
#
# 2007-05-03 ML - Move t/query_cache_sql_prepare.test
#                 to   include/query_cache_sql_prepare.inc
#               - Create two toplevel tests sourcing this routine
#               - Add tests checking that
#                 - another connection gets the same amount of QC hits
#                 - statements running via ps-protocol do not hit QC results
#                   of preceding sql EXECUTEs
#

--source include/have_query_cache.inc
# embedded can't make more than one connection, which this test needs
-- source include/not_embedded.inc
set GLOBAL query_cache_type=ON;
set LOCAL query_cache_type=ON;

--disable_ps2_protocol

connect (con1,localhost,root,,test,$MASTER_MYPORT,);
connection default;

set @initial_query_cache_size = @@global.query_cache_size;
set @@global.query_cache_size=102400;
flush global status;
--disable_warnings
drop table if exists t1;
--enable_warnings
create table t1(c1 int);
insert into t1 values(1),(10),(100);

# First, prepared statements with no parameters
prepare stmt1 from "select * from t1 where c1=10";
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
# Another prepared statement (same text, same connection), should hit the QC
prepare stmt2 from "select * from t1 where c1=10";
execute stmt2;
show status like 'Qcache_hits';
execute stmt2;
show status like 'Qcache_hits';
execute stmt2;
show status like 'Qcache_hits';
# Another prepared statement (same text, other connection), should hit the QC
connection con1;
prepare stmt3 from "select * from t1 where c1=10";
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
connection default;

# Mixup tests, where statements without PREPARE.../EXECUTE.... meet statements
# with PREPARE.../EXECUTE.... (text protocol). Both statements have the
# same text. QC hits occur only when both statements use the same protocol.
# The outcome of the test depends on the mysqltest startup options
# - with "--ps-protocol"
#   Statements without PREPARE.../EXECUTE.... run as prepared statements
#   with binary protocol. Expect to get no QC hits.
# - without any "--<whatever>-protocol"
#   Statements without PREPARE.../EXECUTE run as non prepared statements
#   with text protocol. Expect to get QC hits.
############################################################################
#
# Statement with PREPARE.../EXECUTE.... first
let $my_stmt= SELECT * FROM t1 WHERE c1 = 100;
eval prepare stmt10 from "$my_stmt";
show status like 'Qcache_hits';
execute stmt10;
show status like 'Qcache_hits';
execute stmt10;
show status like 'Qcache_hits';
eval $my_stmt;
show status like 'Qcache_hits';
connection con1;
eval $my_stmt;
show status like 'Qcache_hits';
connection default;
#
# Statement without PREPARE.../EXECUTE.... first
let $my_stmt= SELECT * FROM t1 WHERE c1 = 1;
eval prepare stmt11 from "$my_stmt";
connection con1;
eval prepare stmt12 from "$my_stmt";
connection default;
eval $my_stmt;
show status like 'Qcache_hits';
eval $my_stmt;
show status like 'Qcache_hits';
execute stmt11;
show status like 'Qcache_hits';
connection con1;
execute stmt12;
show status like 'Qcache_hits';
connection default;

# Query caching also works when statement has parameters
# (BUG#29318 Statements prepared with PREPARE and with one parameter don't use
# query cache)
prepare stmt1 from "select * from t1 where c1=?";
show status like 'Qcache_hits';
set @a=1;
execute stmt1 using @a;
show status like 'Qcache_hits';
execute stmt1 using @a;
show status like 'Qcache_hits';
connection con1;
set @a=1;
prepare stmt4 from "select * from t1 where c1=?";
execute stmt4 using @a;
show status like 'Qcache_hits';
# verify that presence of user variables forbids caching
prepare stmt4 from "select @a from t1 where c1=?";
execute stmt4 using @a;
show status like 'Qcache_hits';
execute stmt4 using @a;
show status like 'Qcache_hits';
connection default;

# See if enabling/disabling the query cache between PREPARE and
# EXECUTE is an issue; the expected result is that the query cache
# will not be used.
# Indeed, decision to read/write the query cache is taken at PREPARE
# time, so if the query cache was disabled at PREPARE time then no
# execution of the statement will read/write the query cache.
# If the query cache was enabled at PREPARE time, but disabled at
# EXECUTE time, at EXECUTE time the query cache internal functions do
# nothing so again the query cache is not read/written. But if the
# query cache is re-enabled before another execution then that
# execution will read/write the query cache.

# QC is enabled at PREPARE
prepare stmt1 from "select * from t1 where c1=10";
# then QC is disabled at EXECUTE
# Expect to see no additional Qcache_hits.
set global query_cache_size=0;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
# The QC is global = affects also other connections.
# Expect to see no additional Qcache_hits.
connection con1;
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
#
# then QC is re-enabled for more EXECUTE.
connection default;
set global query_cache_size=102400;
# Expect to see additional Qcache_hits.
# The fact that the QC was temporary disabled should have no affect
# except that the first execute will not hit results from the
# beginning of the test (because QC has been emptied meanwhile by
# setting its size to 0).
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
# The QC is global = affects also other connections.
connection con1;
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
connection default;
#
# then QC is re-disabled for more EXECUTE.
# Expect to see no additional Qcache_hits.
# The fact that the QC was temporary enabled should have no affect.
set global query_cache_size=0;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
# The QC is global = affects also other connections.
connection con1;
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
#

connection default;
# QC is disabled at PREPARE
set global query_cache_size=0;
prepare stmt1 from "select * from t1 where c1=10";
connection con1;
prepare stmt3 from "select * from t1 where c1=10";
connection default;
# then QC is enabled at EXECUTE
set global query_cache_size=102400;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
execute stmt1;
show status like 'Qcache_hits';
# The QC is global = affects also other connections.
connection con1;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
execute stmt3;
show status like 'Qcache_hits';
connection default;
#
# QC is disabled at PREPARE
set global query_cache_size=0;
prepare stmt1 from "select * from t1 where c1=?";
# then QC is enabled at EXECUTE
set global query_cache_size=102400;
show status like 'Qcache_hits';
set @a=1;
execute stmt1 using @a;
show status like 'Qcache_hits';
set @a=100;
execute stmt1 using @a;
show status like 'Qcache_hits';
set @a=10;
execute stmt1 using @a;
show status like 'Qcache_hits';

--enable_ps2_protocol

drop table t1;
disconnect con1;

#
# Bug #25843 Changing default database between PREPARE and EXECUTE of statement
# breaks binlog.
#
# There were actually two problems discovered by this bug:
#
#   1. Default (current) database is not fixed at the creation time.
#      That leads to wrong output of DATABASE() function.
#
#   2. Database attributes (@@collation_database) are not fixed at the creation
#      time. That leads to wrong resultset.
#
# Binlog breakage and Query Cache wrong output happened because of the first
# problem.
#

--echo ########################################################################
--echo #
--echo # BUG#25843: Changing default database between PREPARE and EXECUTE of
--echo # statement breaks binlog.
--echo #
--echo ########################################################################

###############################################################################

--echo
--echo #
--echo # Check that default database and its attributes are fixed at the
--echo # creation time.
--echo #

# Prepare data structures.

--echo
--disable_warnings
DROP DATABASE IF EXISTS mysqltest1;
DROP DATABASE IF EXISTS mysqltest2;
--enable_warnings

--echo
CREATE DATABASE mysqltest1 COLLATE utf8_unicode_ci;
CREATE DATABASE mysqltest2 COLLATE utf8_general_ci;

--echo
CREATE TABLE mysqltest1.t1(msg VARCHAR(255));
CREATE TABLE mysqltest2.t1(msg VARCHAR(255));

# - Create a prepared statement with mysqltest1 as default database;

--echo

use mysqltest1;

PREPARE stmt_a_1 FROM 'INSERT INTO t1 VALUES(DATABASE())';
PREPARE stmt_a_2 FROM 'INSERT INTO t1 VALUES(@@collation_database)';

# - Execute on mysqltest1.

--echo

EXECUTE stmt_a_1;
EXECUTE stmt_a_2;

# - Execute on mysqltest2.

--echo

use mysqltest2;

EXECUTE stmt_a_1;
EXECUTE stmt_a_2;

# - Check the results;

--echo
SELECT * FROM mysqltest1.t1;

--echo
SELECT * FROM mysqltest2.t1;

# - Drop prepared statements.

--echo
DROP PREPARE stmt_a_1;
DROP PREPARE stmt_a_2;

###############################################################################

--echo
--echo #
--echo # The Query Cache test case.
--echo #

--echo
DELETE FROM mysqltest1.t1;
DELETE FROM mysqltest2.t1;

--echo
INSERT INTO mysqltest1.t1 VALUES('mysqltest1.t1');
INSERT INTO mysqltest2.t1 VALUES('mysqltest2.t1');

--echo
use mysqltest1;
PREPARE stmt_b_1 FROM 'SELECT * FROM t1';

--echo
use mysqltest2;
PREPARE stmt_b_2 FROM 'SELECT * FROM t1';

--echo
EXECUTE stmt_b_1;

--echo
EXECUTE stmt_b_2;

--echo
use mysqltest1;

--echo
EXECUTE stmt_b_1;

--echo
EXECUTE stmt_b_2;

--echo
DROP PREPARE stmt_b_1;
DROP PREPARE stmt_b_2;

# Cleanup.

--echo
use test;

--echo
DROP DATABASE mysqltest1;
DROP DATABASE mysqltest2;

###############################################################################

--echo
--echo #
--echo # Check that prepared statements work properly when there is no current
--echo # database.
--echo #

--echo
CREATE DATABASE mysqltest1 COLLATE utf8_unicode_ci;
CREATE DATABASE mysqltest2 COLLATE utf8_general_ci;

--echo
use mysqltest1;

--echo
PREPARE stmt_c_1 FROM 'SELECT DATABASE(), @@collation_database';

--echo
use mysqltest2;

--echo
PREPARE stmt_c_2 FROM 'SELECT DATABASE(), @@collation_database';

--echo
DROP DATABASE mysqltest2;

--echo
SELECT DATABASE(), @@collation_database;

# -- Here we have: current db: NULL; stmt db: mysqltest1;
--echo
EXECUTE stmt_c_1;

--echo
SELECT DATABASE(), @@collation_database;

# -- Here we have: current db: NULL; stmt db: mysqltest2 (non-existent);
--echo
EXECUTE stmt_c_2;

--echo
SELECT DATABASE(), @@collation_database;

# -- Create prepared statement, which has no current database.

--echo
PREPARE stmt_c_3 FROM 'SELECT DATABASE(), @@collation_database';

# -- Here we have: current db: NULL; stmt db: NULL;
--echo
EXECUTE stmt_c_3;

--echo
use mysqltest1;

# -- Here we have: current db: mysqltest1; stmt db: mysqltest2 (non-existent);
--echo
EXECUTE stmt_c_2;

--echo
SELECT DATABASE(), @@collation_database;

# -- Here we have: current db: mysqltest1; stmt db: NULL;
--echo
EXECUTE stmt_c_3;

--echo
SELECT DATABASE(), @@collation_database;

--echo
DROP DATABASE mysqltest1;

--echo
use test;

--echo
--echo ########################################################################
--echo #
--echo # Bug#27430 Crash in subquery code when in PS and table DDL changed
--echo # after PREPARE
--echo # Check the effect of automatic reprepare on query cache
--echo #
--echo ########################################################################
--disable_warnings
drop table if exists t1;
--enable_warnings
create table t1 (a varchar(255));
insert into t1 (a) values ("Pack my box with five dozen liquor jugs.");
flush global status;
prepare stmt from "select a from t1";
execute stmt;
set @@global.query_cache_size=0;
alter table t1 add column b int;
execute stmt;
set @@global.query_cache_size=102400;
execute stmt;
execute stmt;
--echo #
--echo # Sic: ALTER TABLE caused an automatic reprepare 
--echo # of the prepared statement. Since the query cache was disabled
--echo # at the time of reprepare, the new prepared statement doesn't
--echo # work with it.
--echo # 
show status like 'Qcache_hits';
show status like 'Qcache_queries_in_cache';
--echo # Cleanup
deallocate prepare stmt;
drop table t1;

###############################################################################

set @@global.query_cache_size=@initial_query_cache_size;
flush status; # reset Qcache status variables for next tests
set GLOBAL query_cache_type=default;
