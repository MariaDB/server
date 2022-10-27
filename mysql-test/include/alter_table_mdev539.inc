--echo #
--echo # mdev-539: fast build of unique/primary indexes for MyISAM/Aria
--echo #

--source include/default_charset.inc

call mtr.add_suppression("Can't find record in '.*'");

--disable_warnings
DROP DATABASE IF EXISTS dbt3_s001;
--enable_warnings
CREATE DATABASE dbt3_s001;

use dbt3_s001;
 
--disable_query_log
--disable_result_log
--disable_warnings
--source include/dbt3_s001.inc
--enable_warnings
--enable_result_log
--enable_query_log


drop index `primary` on lineitem;
show create table lineitem;
alter table lineitem add primary key (l_orderkey, l_linenumber);
show create table lineitem;
drop index `primary` on lineitem;
select * from lineitem where l_orderkey=1 and l_linenumber=2;
insert into lineitem values
(1,68,9,2,36,34850.16,0.07,0.06,'N','O','1996-04-12','1996-02-28','1996-04-20','TAKE BACK RETURN','MAIL','slyly bold pinto beans detect s');
select * from lineitem where l_orderkey=1 and l_linenumber=2;
--error ER_DUP_ENTRY
alter table lineitem add primary key (l_orderkey, l_linenumber);
show create table lineitem;
select * from lineitem where l_orderkey=1 and l_linenumber=2;
delete from lineitem where l_orderkey=1 and l_linenumber=2 and l_discount=0.07;
alter table lineitem add primary key (l_orderkey, l_linenumber);
show create table lineitem;
select * from lineitem where l_orderkey=1 and l_linenumber=2;

create unique index i_c_name on customer(c_name);
show create table customer;
select * from customer where c_name='Customer#000000003';
drop index i_c_name on customer;
insert into customer values
(303,'Customer#000000003','MG9kdTD2WBHm',1,'11-719-748-3364',7498.12,'AUTOMOBILE','special packages wake. slyly reg');
select * from customer where c_name='Customer#000000003';
--error ER_DUP_ENTRY
alter table customer add unique index i_c_name(c_name);
show create table customer;
select * from customer where c_name='Customer#000000003';
delete from customer where c_custkey=303; 
select * from customer where c_name='Customer#000000003';
alter table customer add unique index i_c_name(c_name);
show create table customer;
select * from customer where c_name='Customer#000000003';

drop index `primary` on customer;
show create table customer;
insert into customer values
(3,'Customer#000000303','MG9kdTD2WBHm',1,'11-719-748-3364',7498.12,'AUTOMOBILE','special packages wake. slyly reg');
alter ignore table customer add primary key (c_custkey);
show create table customer;
select * from customer where c_custkey=3; 
--source include/restore_charset.inc
DROP DATABASE dbt3_s001;
 
