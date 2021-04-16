--echo #
--echo # Testing savepoints with handlers that supports it
--echo #
connect(con1, localhost, root,,);
connect(con2, localhost, root,,);
connect(con3, localhost, root,,);
connection default;
--echo #
--echo # ROLLBACK TO SAVEPOINT releases transactional locks,
--echo # but has no effect on open HANDLERs
--echo #
create table t1 (a int, key a (a));
insert into t1 (a) values (1), (2), (3), (4), (5);
create table t2 like t1;
create table t3 like t1;
begin;
--echo # Have something before the savepoint
select * from t3;
savepoint sv;
handler t1 open;
handler t1 read a first;
handler t1 read a next;
select * from t2;
connection con1;
--echo # Sending:
--send drop table t1
connection con2;
--echo # Sending:
--send drop table t2
connection default;
--echo # Let DROP TABLE statements sync in. We must use
--echo # a separate connection for that, because otherwise SELECT
--echo # will auto-close the HANDLERs, becaues there are pending
--echo # exclusive locks against them.
connection con3;
--echo # Waiting for 'drop table t1' to get blocked...
let $wait_condition=select count(*)=1 from information_schema.processlist
                    where state='Waiting for table metadata lock' and
                          info='drop table t1';
--source include/wait_condition.inc
--echo # Waiting for 'drop table t2' to get blocked...
let $wait_condition=select count(*)=1 from information_schema.processlist
                    where state='Waiting for table metadata lock' and
                          info='drop table t2';
--source include/wait_condition.inc
--echo # Demonstrate that t2 lock was released and t2 was dropped
--echo # after ROLLBACK TO SAVEPOINT
connection default;
rollback to savepoint sv;
connection con2;
--echo # Reaping 'drop table t2'...
--reap
--echo # Demonstrate that ROLLBACK TO SAVEPOINT didn't release the handler
--echo # lock.
connection default;
handler t1 read a next;
handler t1 read a next;
--echo # Demonstrate that the drop will go through as soon as we close the 
--echo # HANDLER
handler t1 close;
connection con1;
--echo # Reaping 'drop table t1'...
--reap
connection default;
commit;
drop table t3;
--echo #
--echo # A few special cases when using SAVEPOINT/ROLLBACK TO
--echo # SAVEPOINT and HANDLER.
--echo #
--echo # Show that rollback to the savepoint taken in the beginning
--echo # of the transaction doesn't release mdl lock on
--echo # the HANDLER that was opened later.
--echo #
create table t1 (a int, key using btree (a));
insert into t1 (a) values (1), (2), (3), (4), (5);
create table t2 like t1;
begin;
savepoint sv;
handler t1 open;
handler t1 read a first;
handler t1 read a next;
select * from t2;
connection con1;
--echo # Sending:
--send drop table t1
connection con2;
--echo # Sending:
--send drop table t2
connection default;
--echo # Let DROP TABLE statements sync in. We must use
--echo # a separate connection for that, because otherwise SELECT
--echo # will auto-close the HANDLERs, becaues there are pending
--echo # exclusive locks against them.
connection con3;
--echo # Waiting for 'drop table t1' to get blocked...
let $wait_condition=select count(*)=1 from information_schema.processlist
                    where state='Waiting for table metadata lock' and
                          info='drop table t1';
--source include/wait_condition.inc
--echo # Waiting for 'drop table t2' to get blocked...
let $wait_condition=select count(*)=1 from information_schema.processlist
                    where state='Waiting for table metadata lock' and
                          info='drop table t2';
--source include/wait_condition.inc
--echo # Demonstrate that t2 lock was released and t2 was dropped
--echo # after ROLLBACK TO SAVEPOINT
connection default;
rollback to savepoint sv;
connection con2;
--echo # Reaping 'drop table t2'...
--reap
--echo # Demonstrate that ROLLBACK TO SAVEPOINT didn't release the handler
--echo # lock.
connection default;
handler t1 read a next;
handler t1 read a next;
--echo # Demonstrate that the drop will go through as soon as we close the 
--echo # HANDLER
handler t1 close;
connection con1;
--echo # Reaping 'drop table t1'...
--reap
connection default;
commit;
--echo #
--echo # Show that rollback to the savepoint taken in the beginning
--echo # of the transaction works properly (no valgrind warnins, etc),
--echo # even though it's done after the HANDLER mdl lock that was there
--echo # at the beginning is released and added again.
--echo #
create table t1 (a int, key using btree (a));
insert into t1 (a) values (1), (2), (3), (4), (5);
create table t2 like t1;
create table t3 like t1;
insert into t3 (a) select a from t1;
begin;
handler t1 open;
savepoint sv;
handler t1 read a first;
select * from t2;
handler t1 close;
handler t3 open;
handler t3 read a first;
rollback to savepoint sv;
connection con1;
drop table t1, t2;
--echo # Sending:
--send drop table t3
--echo # Let DROP TABLE statement sync in.
connection con2;
--echo # Waiting for 'drop table t3' to get blocked...
let $wait_condition=select count(*)=1 from information_schema.processlist
                    where state='Waiting for table metadata lock' and
                          info='drop table t3';
--source include/wait_condition.inc
--echo # Demonstrate that ROLLBACK TO SAVEPOINT didn't release the handler
--echo # lock.
connection default;
handler t3 read a next;
--echo # Demonstrate that the drop will go through as soon as we close the 
--echo # HANDLER
handler t3 close;
connection con1;
--echo # Reaping 'drop table t3'...
--reap
connection default;
commit;

--echo #
--echo # Cleanup for savepoint.inc
--echo #
connection con1;
disconnect con1;
--source include/wait_until_disconnected.inc
connection con2;
disconnect con2;
--source include/wait_until_disconnected.inc
connection con3;
disconnect con3;
--source include/wait_until_disconnected.inc
connection default;
