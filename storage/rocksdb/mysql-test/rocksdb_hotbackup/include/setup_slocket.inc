connection server_1;
create database slocket;

create table slocket.t1 (
  `id` int(10) not null auto_increment,
  `k` int(10),
  `data` varchar(2048),
  primary key (`id`),
  key (`k`)
) engine=rocksdb;
