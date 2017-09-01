--source include/have_rocksdb.inc

DELIMITER //;
CREATE PROCEDURE bloom_start()
BEGIN
 select variable_value into @c from information_schema.global_status where variable_name='rocksdb_bloom_filter_prefix_checked';
 select variable_value into @u from information_schema.global_status where variable_name='rocksdb_bloom_filter_prefix_useful';
END//
CREATE PROCEDURE bloom_end()
BEGIN
select case when variable_value-@c > 0 then 'true' else 'false' end as checked from information_schema.global_status where variable_name='rocksdb_bloom_filter_prefix_checked';
END//
DELIMITER ;//

#BF is sometimes invoked and useful
--let $CF=
--source bloomfilter_table_def.inc
--source bloomfilter_load_select.inc

#BF is always invoked but not useful at all
--let $CF=COMMENT 'cf_short_prefix'
--source bloomfilter_table_def.inc
--source bloomfilter_load_select.inc

--let $CF=COMMENT 'rev:cf_short_prefix'
--source bloomfilter_table_def.inc
--source bloomfilter_load_select.inc

#BF is most of the time invoked and useful
--let $CF=COMMENT 'cf_long_prefix'
--source bloomfilter_table_def.inc
--source bloomfilter_load_select.inc

--let $CF=COMMENT 'rev:cf_long_prefix'
--source bloomfilter_table_def.inc
--source bloomfilter_load_select.inc


# BUG: Prev() with prefix lookup should not use prefix bloom filter
create table r1 (id1 bigint, id2 bigint, id3 bigint, v1 int, v2 text, primary key (id1, id2, id3)) engine=rocksdb DEFAULT CHARSET=latin1 collate latin1_bin;
--disable_query_log
let $max = 100;
let $i = 1;

create table t10(a int primary key);
insert into t10 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

create table t11(a int primary key);
insert into t11 select A.a + B.a* 10 + C.a * 100 from t10 A, t10 B, t10 C;

set @a=0;
set rocksdb_bulk_load=1;
while ($i <= $max) {
 inc $i;
 eval insert into r1 select @a:=@a+1, @a, @a, @a, @a from t11;
}
set rocksdb_bulk_load=0;

drop table t10, t11;

--enable_query_log
call bloom_start();
select * from r1 where id1=1 and id2 in (1) order by id3 asc;
call bloom_end();
call bloom_start();
select * from r1 where id1=1 and id2 in (1) order by id3 desc;
call bloom_end();

# cleanup
DROP PROCEDURE bloom_start;
DROP PROCEDURE bloom_end;
truncate table t1;
optimize table t1;
truncate table t2;
optimize table t2;
drop table if exists t1;
drop table if exists t2;
drop table if exists r1;
