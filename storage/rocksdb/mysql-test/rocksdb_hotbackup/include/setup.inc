--let $rpl_server_count= 2
--let $rpl_topology= none
--source include/rpl_init.inc
--source include/rpl_default_connections.inc

connection server_1;
create database db1;

create table db1.t1 (
  `id` int(10) not null auto_increment,
  `k` int(10),
  `data` varchar(2048),
  primary key (`id`),
  key (`k`)
) engine=rocksdb;

