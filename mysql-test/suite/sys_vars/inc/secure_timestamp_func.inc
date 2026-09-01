#
# MDEV-15923 option to control who can set session @@timestamp
#
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
connect con2,127.0.0.1,foo,,"*NO-ONE*",$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'SUPER';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;

--echo ### BINLOG REPLAY
create user foo@127.0.0.1;
grant binlog replay on *.* TO `foo`@`127.0.0.1`;
connect con2,127.0.0.1,foo,,"*NO-ONE*",$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'BINLOG REPLAY';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;

--echo ### non-privileged user
create user foo@127.0.0.1;
connect con2,127.0.0.1,foo,,"*NO-ONE*",$SLAVE_MYPORT;
disable_abort_on_error;
set timestamp=1234567890.101112;
enable_abort_on_error;
select if(now(6) > 20100101, 'READONLY', 'EDITABLE') as 'non-privileged';
disconnect con2;
connection slave;
drop user foo@127.0.0.1;

### replication
connection master;
set time_zone='+00:00';
set timestamp=1234567890.101112;
select @@timestamp, now(6);

create table t1 (b varchar(20) primary key, a timestamp(6) default current_timestamp(6) on update current_timestamp(6));
insert t1 (b) values ('inserted'), ('');
update t1 set b='updated' where b='';
sync_slave_with_master;
create trigger t1rbr before insert on t1 for each row set new.a=now(6);
set @@global.slave_run_triggers_for_rbr= yes;
binlog 'LQfqWg8BAAAA/AAAAAABAAABAAQAMTAuMy42LU1hcmlhREItZGVidWctbG9nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAtB+paEzgNAAgAEgAEBAQEEgAA5AAEGggAAAAICAgCAAAACgoKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEEwQADQgICAoKCgFlBcaR';
binlog '0gKWSRMBAAAAMQAAAHQDAAAAAB8AAAAAAAEABHRlc3QAAnQxAAIPEQMUAAYBQFUzwA==0gKWSRcBAAAAMAAAAKQDAAAAAB8AAAAAAAEAAv/8BmJpbmxvZ0mWAtIBivg3mwo+';
set timestamp=default;
set @@global.slave_run_triggers_for_rbr= default;
select b, if(a > 20100101, 'READONLY', 'EDITABLE') as 'REPLICATION' from t1;
connection master;
## the above was generated with:
#set binlog_format=row;
#insert t1 (b) values ('binlog');
#let datadir=`select @@datadir`;
#exec $MYSQL_BINLOG $datadir/master-bin.000001;
drop table t1;

create table t1 (b varchar(20) primary key, rs timestamp(6) as row start, re timestamp(6) as row end,
  period for system_time (rs,re)) with system versioning;
insert t1 (b) values ('inserted'), ('before update');
set timestamp=1345678900.101112;
set binlog_format=statement; # see comments in MDEV-23486
update t1 set b='updated...' where b<'c';
select * from t1 for system_time all;
sync_slave_with_master;
select b, strcmp(year(rs), 2012), strcmp(year(re),year(current_date)) from t1 for system_time all;
connection master;
drop table t1;

source include/rpl_end.inc;
