#
# MDEV-15923 option to control who can set session @@timestamp
#
source include/have_binlog_format_statement.inc;
source include/master-slave.inc;

connection slave;
select @@secure_timestamp;

--echo ### ALL PRIVILEGES
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'ALL PRIVILEGES';
set timestamp=default;

--echo ### SUPER
create user foo@127.0.0.1;
grant super on *.* TO `foo`@`127.0.0.1`;
connect con2,127.0.0.1,foo,,test,$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'SUPER';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;
set timestamp=default;

--echo ### BINLOG REPLAY
create user foo@127.0.0.1;
grant binlog replay on *.* TO `foo`@`127.0.0.1`;
connect con2,127.0.0.1,foo,,test,$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'BINLOG REPLAY';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;
set timestamp=default;

--echo ### non-privileged user
create user foo@127.0.0.1;
connect con2,127.0.0.1,foo,,test,$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'non-privileged';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;
set timestamp=default;

### replication
connection master;
set time_zone='+00:00';
set timestamp=1234567890.101112;
select @@timestamp, now(6);

create table t1 (b varchar(20), a timestamp(6) default current_timestamp(6));
insert t1 (b) values ('replicated');
sync_slave_with_master;
create trigger t1rbr before insert on t1 for each row set new.a=now(6);
set @@global.slave_run_triggers_for_rbr= yes;
binlog 'LQfqWg8BAAAA/AAAAAABAAABAAQAMTAuMy42LU1hcmlhREItZGVidWctbG9nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAtB+paEzgNAAgAEgAEBAQEEgAA5AAEGggAAAAICAgCAAAACgoKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEEwQADQgICAoKCgFlBcaR';
binlog '0gKWSRMBAAAAMQAAAHQDAAAAAB8AAAAAAAEABHRlc3QAAnQxAAIPEQMUAAYBQFUzwA==0gKWSRcBAAAAMAAAAKQDAAAAAB8AAAAAAAEAAv/8BmJpbmxvZ0mWAtIBivg3mwo+';
set @@global.slave_run_triggers_for_rbr= default;
select b, if(a > 20100101, 'READONLY', 'EDITABLE') as 'REPLICATION' from t1;
connection master;
#set binlog_format=row;
#insert t1 (b) values ('binlog');
#let datadir=`select @@datadir`;
#exec $MYSQL_BINLOG $datadir/master-bin.000001;
drop table t1;

source include/rpl_end.inc;
