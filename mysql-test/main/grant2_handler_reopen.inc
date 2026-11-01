# Forces HANDLER tables of connection con1 to be reopened on next read:
# $ddl (exclusive MDL) waits for con1 to close the handler table, which
# happens when con1 opens any other table.
# Expects connection con1 to exist and con2 not to exist.

connect con2, localhost, root;
send_eval $ddl;
connection default;
let $wait_condition= select count(*) = 1 from information_schema.processlist
  where state = 'Waiting for table metadata lock' and info = '$ddl';
--source include/wait_condition.inc
connection con1;
select 1 from information_schema.character_sets limit 1;
connection con2;
reap;
disconnect con2;
connection con1;
