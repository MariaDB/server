--echo #
--echo # Testing concurrent transactions.
--echo #

--source include/count_sessions.inc
connect (con1,localhost,root,,);
connect (con2,localhost,root,,);
connect (con3,localhost,root,,);

connection con1;
begin;
insert into t values (); # 1

connection con2;
begin;
insert into t values (); # 2

connection con3;
begin;
insert into t values (); # 3

connection con1;
insert into t values (); # 4

connection con2;
insert into t values (); # 5

connection con3;
insert into t values (); # 6

connection con2;
commit;

connection con3;
rollback;

connection con1;
commit;

delete from t;

--echo # Master value before restart
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Slave value before restart
sync_slave_with_master;
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

connection slave;
--source include/stop_slave.inc
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc

connection default;
--echo # Master value after restart
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--let $rpl_server_number = 2
--source include/rpl_restart_server.inc

connection slave;
--source include/start_slave.inc
--echo # Slave value after restart
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

disconnect con1;
disconnect con2;
disconnect con3;
--source include/wait_until_count_sessions.inc

--echo #
--echo # Testing interaction of merge markers with various DDL statements.
--echo #
connection slave;
--source include/stop_slave.inc

connection default;

--echo # Drop and add primary key.
alter table t modify i int;
alter table t drop primary key;
alter table t add primary key (i);
alter table t modify i int auto_increment;

--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Remove auto_increment property.
alter table t modify i int;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Add auto_increment property.
insert into t values (123);
alter table t modify i int auto_increment;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Add column j.
alter table t add column j int;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Rename tables.
rename table t to t2;
rename table t2 to t;

--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Change auto_increment property
alter table t auto_increment = 1000;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

alter table t auto_increment = 1;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

alter table t drop primary key, add key (i), auto_increment = 1;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

alter table t add key (j), auto_increment = 1;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

alter table t modify i int;
alter table t add column (k int auto_increment), add key(k), auto_increment=15;
--let $rpl_server_number = 1
--source include/rpl_restart_server.inc
select table_schema, table_name, auto_increment from information_schema.tables where table_name = 't';

--echo # Drop table.
drop table t;

--let $rpl_server_number = 1
--source include/rpl_restart_server.inc

connection slave;
--source include/start_slave.inc
