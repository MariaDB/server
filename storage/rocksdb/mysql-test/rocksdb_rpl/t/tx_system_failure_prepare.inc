create table t1 (id int primary key, value int) engine = rocksdb;

insert into t1 values (1,1),(2,2),(3,3);

eval set session debug= $myrocks_debug_set_code;
--error ER_INTERNAL_ERROR
insert into t1 values (4,4);

begin;
insert into t1 values (5,5);
insert into t1 values (6,6);
--error ER_INTERNAL_ERROR
commit;
eval set session debug= $myrocks_debug_unset_code;
select * from t1 order by id;

drop table t1;
