# Replication of character sets.
# This test will fail if the server/client does not support enough charsets.

source include/master-slave.inc;
set timestamp=1000000000;

create database mysqltest2 character set latin2;
set @@character_set_server=latin5;      
create database mysqltest3;
show create database mysqltest2;
show create database mysqltest3;
sync_slave_with_master;
show create database mysqltest2;
show create database mysqltest3;

connection master;
set @@collation_server=armscii8_bin;
drop database mysqltest3;
create database mysqltest3;
show create database mysqltest3;
sync_slave_with_master;
show create database mysqltest3;

connection master;
use mysqltest2;
create table t1 (a int auto_increment primary key, b varchar(100));
set character_set_client=cp850, collation_connection=latin2_croatian_ci;
insert into t1 (b) values(@@character_set_server);
insert into t1 (b) values(@@collation_server);
# character_set_database and collation_database are not tested as they
# needn't be replicated (Bar said in Jan 2005).
insert into t1 (b) values(@@character_set_client);
# collation_client does not exist
insert into t1 (b) values(@@character_set_connection);
insert into t1 (b) values(@@collation_connection);
select * from t1 order by a;
sync_slave_with_master;
select * from mysqltest2.t1 order by a;

connection master;
set character_set_client=latin1, collation_connection=latin1_german1_ci;
truncate table t1;
insert into t1 (b) values(@@collation_connection);
insert into t1 (b) values(LEAST("M�ller","Muffler"));
set collation_connection=latin1_german2_ci;
insert into t1 (b) values(@@collation_connection);
insert into t1 (b) values(LEAST("M�ller","Muffler"));
select * from t1 order by a;
sync_slave_with_master;
select * from mysqltest2.t1 order by a;

# Presently charset info is not logged with LOAD DATA but it will
# change in Jan 2005 when Dmitri pushes his new LOAD DATA,
# before 5.0.3 goes out. When done, LOAD DATA INFILE should be tested
# here.

# See if user var is prefixed with collation in binlog and replicated well.
# Note: replication of user variables is broken as far as derivation is
# concerned. That's because when we store a user variable in the binlog,
# we lose its derivation. So later on the slave, it's impossible to
# know if the collation was explicit or not, so we use DERIVATION_NONE,
# which provokes error messages (like 'Illegal mix of collation') when
# we replay the master's INSERT/etc statements.
connection master;
set @a= _cp850 'M�ller' collate cp850_general_ci;
truncate table t1;
insert into t1 (b) values(collation(@a));
select * from t1 order by a;
sync_slave_with_master;
select * from mysqltest2.t1 order by a;

connection master;
drop database mysqltest2;
drop database mysqltest3;
sync_slave_with_master;

# Check that we can change global.collation_server (since 5.0.3)

set global character_set_server=latin2;
set global character_set_server=latin1; # back
connection master;
set global character_set_server=latin2;
set global character_set_server=latin1; # back

# Test of wrong character set numbers;
error 1115;
set character_set_client=9999999;
error 1273;
set collation_server=9999998;

# This one was contributed by Sergey Petrunia (BUG#3943)

use test;
CREATE TABLE t1 (c1 VARBINARY(255), c2 VARBINARY(255));
SET CHARACTER_SET_CLIENT=koi8r,
 CHARACTER_SET_CONNECTION=cp1251, 
 CHARACTER_SET_RESULTS=koi8r;
INSERT INTO t1 (c1, c2) VALUES ('��, �� �������','��, �� �������');
select hex(c1), hex(c2) from t1;
sync_slave_with_master;
select hex(c1), hex(c2) from t1;

connection master;
drop table t1;
sync_slave_with_master;

#
#  BUG#6676: Derivation of variables must be correct on slave
#
connection master;
eval create table `t1` (
  `pk` varchar(10) not null default '', 
  primary key (`pk`)
) engine=$engine_type default charset=latin1;
set @p=_latin1 'test';
update t1 set pk='test' where pk=@p;
drop table t1;

# End of 4.1 tests


--echo #
--echo # Start of 10.3 tests
--echo #


--echo #
--echo # MDEV-12685 Oracle-compatible function CHR()
--echo #

connection master;
CREATE DATABASE db1 DEFAULT CHARACTER SET latin1 COLLATE latin1_bin;
USE db1;
CREATE TABLE t1 AS SELECT CHR(0x60);
sync_slave_with_master;
SHOW CREATE TABLE db1.t1;
connection master;
USE test;
DROP DATABASE db1;
sync_slave_with_master;


--echo #
--echo # End of 10.3 tests
--echo #

# The above tests change global and session @@collation_server
# Restore them back to avoid internal check failures.

connection master;
SET GLOBAL collation_server=utf8mb4_uca1400_ai_ci;
SET SESSION collation_server=utf8mb4_uca1400_ai_ci;
connection slave;
SET GLOBAL collation_server=utf8mb4_uca1400_ai_ci;
SET SESSION collation_server=utf8mb4_uca1400_ai_ci;

--source include/rpl_end.inc
