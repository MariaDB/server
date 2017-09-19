###############################################################################
# Common test file for high priority DDL
###############################################################################


create user test_user1@localhost;
grant all on test to test_user1@localhost;
create user test_user2@localhost;
grant all on test to test_user2@localhost;

# Default values
--let $con_block = con1
--let $con_kill = default
--let $should_kill = 1
--let $recreate_table = 1
--let $throw_error = 1

##
## killing conflicting shared locks by alter table
##

--let $blocking_sql = lock tables t1 read;
--let $cmd = alter table t1 modify i bigint;
--let $high_priority_cmd = alter high_priority table t1 modify i bigint;

--source include/ddl_high_priority_module.inc

##
## killing conflicting shared lock in a transaction
## transaction will rollback
##

--let $blocking_sql = begin; insert into t1 values (4); select i from t1;
--let $cmd = alter table t1 rename t1_new;
--let $high_priority_cmd = alter high_priority table t1 rename t1_new;

--source include/ddl_high_priority_module.inc

select * from t1_new;
drop table t1_new;

##
## simulate conflicting DDL which will not be killed
##

# Simulate conflicting DDL
# This will hold MDL_SHARED_NO_READ_WRITE, which may be upgraded to exclusive
# locks to run DDLs like ALTER TABLE
# the upgradable/exclusive lock should not be killed

--let $should_kill = 0

--let $blocking_sql = lock tables t1 write;
--let $cmd = drop table t1;
--let $high_priority_cmd = drop high_priority table t1;

--source include/ddl_high_priority_module.inc

# restore $should_kill
--let $should_kill = 1

##
## killing conflicting transaction by drop table DDL
##

--let $blocking_sql = lock tables t1 read; begin; insert into t1 values (4);
--let $cmd = drop table t1;
--let $high_priority_cmd = drop high_priority table t1;

--source include/ddl_high_priority_module.inc

##
## no effect for regular users
##

connect (con2,localhost,test_user2,,test,,);
# $con_kill is regular user
--let $con_kill = con2
--let $should_kill = 0

--let $blocking_sql = lock tables t1 read;
--let $cmd = alter table t1 modify i bigint;
--let $high_priority_cmd = alter high_priority table t1 modify i bigint;

--source include/ddl_high_priority_module.inc

disconnect con2;

# restore $con_kill
--let $con_kill = default
# restore $should_kill
--let $should_kill = 1

##
## create/drop index
##

# create index

--let $blocking_sql = lock tables t1 read;
--let $cmd = create index idx1 on t1 (i);
--let $high_priority_cmd = create high_priority index idx1 on t1 (i);

--source include/ddl_high_priority_module.inc

# drop index (use the previously created table)
--let $recreate_table = 0

--let $cmd = drop index idx1 on t1;
--let $high_priority_cmd = drop high_priority index idx1 on t1;

--source include/ddl_high_priority_module.inc

# restore $recreate_table
--let $recreate_table = 1

##
## high_priority truncate table
##

--let $blocking_sql = lock tables t1 read;
--let $cmd = truncate t1;
--let $high_priority_cmd = truncate high_priority t1;

--source include/ddl_high_priority_module.inc

##
## high_priority create/drop trigger
##

--let $blocking_sql = lock tables t1 read;
--let $cmd = create trigger ins_sum before insert on t1 for each row set @sum = @sum + new.i;
--let $high_priority_cmd = create high_priority trigger ins_sum before insert on t1 for each row set @sum = @sum + new.i;

--source include/ddl_high_priority_module.inc

# drop trigger (use the previously created table)
--let $recreate_table = 0

--let $cmd = drop trigger ins_sum;
--let $high_priority_cmd = drop high_priority trigger ins_sum;

--source include/ddl_high_priority_module.inc

# restore $recreate_table
--let $recreate_table = 1

##
## high_priority optimize table
##
## "optimize table" doesn't throw errors. It catches all errors, and
## returns a result set in a table
##

--let $throw_error = 0

--let $blocking_sql = lock tables t1 read;
--let $cmd = optimize table t1;
--let $high_priority_cmd = optimize high_priority table t1;

--source include/ddl_high_priority_module.inc

# restore throw_error
--let $throw_error = 1

##
## clean up
##

drop user test_user1@localhost;
drop user test_user2@localhost;
--disable_warnings
drop table if exists t1;
--enable_warnings
