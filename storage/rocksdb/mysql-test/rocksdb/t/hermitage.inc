--enable_connect_log

# Save the initial number of concurrent sessions
--source include/count_sessions.inc

### See full test cases here:
### https://github.com/ept/hermitage/blob/master/mysql.md

--disable_warnings
DROP TABLE IF EXISTS test;
--enable_warnings

connect (con1,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;
connect (con2,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;
connect (con3,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;

connection con1;

create table test (id int primary key, value int) engine=rocksdb;

### Prevents Aborted Reads (G1a)
--source hermitage_init.inc

connection con1;
select * from test;
update test set value = 101 where id = 1;
connection con2;
select * from test; # Still shows 1 => 10
connection con1;
rollback;
connection con2; # Still shows 1 => 10
select * from test;
commit;


### Prevents Intermediate Reads (G1b)
--source hermitage_init.inc

connection con1;
update test set value = 101 where id = 1;
connection con2;
select * from test; # Still shows 1 => 10
connection con1;
update test set value = 11 where id = 1;
commit;
connection con2;
select * from test; # Now shows 1 => 11
commit;


### Prevents Circular Information Flow (G1c)
--source hermitage_init.inc

connection con1;
update test set value = 11 where id = 1;
connection con2;
update test set value = 22 where id = 2;
connection con1;
select * from test where id = 2; # Still shows 2 => 20
connection con2;
select * from test where id = 1; # Still shows 1 => 10
connection con1;
commit;
connection con2;
commit;


### prevents Observed Transaction Vanishes (OTV)
--source hermitage_init.inc

connection con1;
update test set value = 11 where id = 1;
update test set value = 19 where id = 2;
connection con2;
send update test set value = 12 where id = 1;
connection con1;
commit;
connection con2;
reap;
connection con3;
select * from test; # Shows 1 => 11, 2 => 19
connection con2;
update test set value = 18 where id = 2;
connection con3;
select * from test; # Shows 1 => 11, 2 => 19
connection con2;
commit;
connection con3;
select * from test; # Shows 1 => 12, 2 => 18
commit;


### Predicate-Many-Preceders (PMP) -- RC does not prevent, RR prevents
--source hermitage_init.inc

connection con1;
select * from test where value = 30;
connection con2;
insert into test (id, value) values(3, 30);
commit;
connection con1;
# RC: Returns the newly inserted row
# RR: Still returns nothing
select * from test where value % 3 = 0;
commit;

--source hermitage_init.inc
connection con1;
update test set value = value + 10;
connection con2;
select variable_value into @a from information_schema.global_status where variable_name='rocksdb_snapshot_conflict_errors';
select * from test;
send delete from test where value = 20;
connection con1;
commit;
connection con2;
if ($trx_isolation == "READ COMMITTED")
{
  reap;
  # RC: Returns 2 => 30
  select * from test;
}
if ($trx_isolation == "REPEATABLE READ")
{
  --error ER_LOCK_DEADLOCK
  reap;
  select variable_value-@a from information_schema.global_status where variable_name='rocksdb_snapshot_conflict_errors';

}
commit;


### Lost Update (P4) -- RC does not prevent, RR prevents
--source hermitage_init.inc

connection con1;
select * from test where id = 1;
connection con2;
select * from test where id = 1;
connection con1;
update test set value = 11 where id = 1;
connection con2;
send update test set value = 12 where id = 1;
connection con1;
commit;
connection con2;
if ($trx_isolation == "READ COMMITTED")
{
  reap;
  # RC: Returns 1 => 12
  select * from test;
}
if ($trx_isolation == "REPEATABLE READ")
{
  --error ER_LOCK_DEADLOCK
  reap;
}
commit;


### Read Skew (G-single) -- RC does not prevent, RR prevents
--source hermitage_init.inc

connection con1;
select * from test where id = 1;
connection con2;
select * from test where id = 1;
select * from test where id = 2;
update test set value = 12 where id = 1;
update test set value = 18 where id = 2;
commit;
connection con1;
select * from test where id = 2; # RC shows 18, RR shows 20
commit;

# test using predicate dependencies
--source hermitage_init.inc

connection con1;
select * from test where value % 5 = 0;
connection con2;
update test set value = 12 where value = 10;
commit;
connection con1;
# RC: returns 1 => 12, RR: returns nothing
select * from test where value % 3 = 0;
commit;

# on a write predicate
--source hermitage_init.inc

connection con1;
select * from test where id = 1;
connection con2;
select * from test;
update test set value = 12 where id = 1;
update test set value = 18 where id = 2;
commit;
connection con1;
if ($trx_isolation == "READ COMMITTED")
{
  delete from test where value = 20; # doesn't delete anything
  select * from test where id = 2; # shows 2 => 18
}
if ($trx_isolation == "REPEATABLE READ")
{
  --error ER_LOCK_DEADLOCK
  delete from test where value = 20;
}
commit;


### Write Skew (G2-item) -- Neither RC/RR prevents
--source hermitage_init.inc

connection con1;
select * from test where id in (1,2);
connection con2;
select * from test where id in (1,2);
connection con1;
update test set value = 11 where id = 1;
connection con2;
update test set value = 21 where id = 2;
connection con1;
commit;
connection con2;
commit;

### Anti-Dependency Cycles (G2) -- Neither RC/RR prevents
--source hermitage_init.inc

connection con1;
select * from test where value % 3 = 0;
connection con2;
select * from test where value % 3 = 0;
connection con1;
insert into test (id, value) values(3, 30);
connection con2;
insert into test (id, value) values(4, 42);
connection con1;
commit;
connection con2;
commit;
select * from test where value % 3 = 0; # Either. Returns 3 => 30, 4 => 42
connection con1;
select * from test where value % 3 = 0;


connection default;
drop table test;

disconnect con1;
disconnect con2;
disconnect con3;
