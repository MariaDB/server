#
# This file tests how privilege are propagated through a complex role graph.
# Here's a graph
#
# role1 ->- role2 -->- role4 -->- role6 ->- role8
#                \               /     \
#                 \->- role5 ->-/       \->- role9 ->- role10 ->- foo@localhost
#                     /     \               /
#           role3 ->-/       \->- role7 ->-/
#
# privilege checks verify that grants/revokes are propagated correctly
# from the role1 to role10. additionally debug status variables verify
# that propagation doesn't do unnecessary work (only touches the
# smallest possible number of nodes and doesn't merge privileges that
# didn't change)
#
source include/not_embedded.inc;

create user foo@localhost;
create role role1;
create role role2;
create role role3;
create role role4;
create role role5;
create role role6;
create role role7;
create role role8;
create role role9;
create role role10;

grant role1 to role2;
grant role2 to role4;
grant role2 to role5;
grant role3 to role5;
grant role4 to role6;
grant role5 to role6;
grant role5 to role7;
grant role6 to role8;
grant role6 to role9;
grant role7 to role9;
grant role9 to role10;
grant role10 to foo@localhost;

# try to create a cycle
--error ER_CANNOT_GRANT_ROLE
grant role10 to role2;

connect (foo, localhost, foo);
--sorted_result
show grants;
--sorted_result
select * from information_schema.applicable_roles;

show status like 'debug%';

#
# global privileges
#
connection default;
grant select on *.* to role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role role10;
select count(*) from mysql.roles_mapping;
--sorted_result
show grants;
--sorted_result
select * from information_schema.enabled_roles;

connection default;
revoke select on *.* from role1;
show status like 'debug%';
connection foo;
# global privileges are cached in the THD, changes don't take effect immediately
select count(*) from mysql.roles_mapping;
set role none;
set role role10;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role none;

#
# database privileges
#
connection default;
grant select on mysql.* to role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role role10;
select count(*) from mysql.roles_mapping;
--sorted_result
show grants;

connection default;
revoke select on mysql.* from role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role none;

#
# table privileges
#

connection default;
grant select on mysql.roles_mapping to role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role role10;
select count(*) from mysql.roles_mapping;
--sorted_result
show grants;

connection default;
revoke select on mysql.roles_mapping from role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role none;

#
# column privileges
#

connection default;
grant select(User) on mysql.roles_mapping to role1;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(*) from mysql.roles_mapping;
set role role10;
--error ER_COLUMNACCESS_DENIED_ERROR
select count(concat(User,Host,Role)) from mysql.roles_mapping;
select count(concat(User)) from mysql.roles_mapping;
--sorted_result
show grants;

connection default;
grant select(Host) on mysql.roles_mapping to role3;
show status like 'debug%';
connection foo;
--error ER_COLUMNACCESS_DENIED_ERROR
select count(concat(User,Host,Role)) from mysql.roles_mapping;
select count(concat(User,Host)) from mysql.roles_mapping;
--sorted_result
show grants;

connection default;
revoke select(User) on mysql.roles_mapping from role1;
show status like 'debug%';
connection foo;
--error ER_COLUMNACCESS_DENIED_ERROR
select count(concat(User,Host)) from mysql.roles_mapping;
select count(concat(Host)) from mysql.roles_mapping;

connection default;
revoke select(Host) on mysql.roles_mapping from role3;
show status like 'debug%';
connection foo;
--error ER_TABLEACCESS_DENIED_ERROR
select count(concat(Host)) from mysql.roles_mapping;
set role none;

#
# routine privileges
#

connection default;
create procedure pr1() select "pr1";
create function fn1() returns char(10) return "fn1";
grant execute on procedure test.pr1 to role1;
show status like 'debug%';
connection foo;
--error ER_PROCACCESS_DENIED_ERROR
call pr1();
set role role10;
call pr1();
--error ER_PROCACCESS_DENIED_ERROR
select fn1();

connection default;
grant execute on function test.fn1 to role5;
show status like 'debug%';
connection foo;
select fn1();

connection default;
revoke execute on procedure test.pr1 from role1;
show status like 'debug%';
connection foo;
--error ER_PROCACCESS_DENIED_ERROR
call pr1();
select fn1();

connection default;
revoke execute on function test.fn1 from role5;
show status like 'debug%';
connection foo;
--error ER_PROCACCESS_DENIED_ERROR
select fn1();
set role none;

connection default;
drop procedure pr1;
drop function fn1;

#
# test shortcuts
#

grant select on mysql.roles_mapping to role3;
show status like 'debug%';
# this grant only propagates to roles role2 and role4,
# and tries to propagate to role5, discovering that it already has it
grant select on mysql.roles_mapping to role1;
show status like 'debug%';
# this only tries to propagate to role5 and exits early
revoke select on mysql.roles_mapping from role3;
show status like 'debug%';
# propagates to all 8 roles, normally
revoke select on mysql.roles_mapping from role1;
show status like 'debug%';

grant select on mysql.* to role1;
show status like 'debug%';
# only entries for `test` are merged, not for `mysql`
grant select on test.* to role1;
show status like 'debug%';
revoke select on mysql.* from role1;
show status like 'debug%';
revoke select on test.* from role1;
show status like 'debug%';

#
# cleanup
#

connection default;
drop user foo@localhost;
drop role role1;
drop role role2;
drop role role3;
drop role role4;
drop role role5;
drop role role6;
drop role role7;
drop role role8;
drop role role9;
drop role role10;

