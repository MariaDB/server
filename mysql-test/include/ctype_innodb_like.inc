#
# Bug#11650: LIKE pattern matching using prefix index
# doesn't return correct result
#
--disable_query_log
set @ctype_save_sql_mode=@@sql_mode;
set sql_mode="";
--enable_query_log
--disable_warnings
#
# This query creates a column using
# character_set_connection and
# collation_connection.
#
create table t1 engine=innodb select repeat('a',50) as c1;
--enable_warnings
--disable_query_log
set sql_mode=@ctype_save_sql_mode;
--enable_query_log
alter table t1 add index(c1(5));

insert into t1 values ('abcdefg'),('abcde100'),('abcde110'),('abcde111');
select collation(c1) from t1 limit 1;
select c1 from t1 where c1 like 'abcdef%' order by c1;
select c1 from t1 where c1 like 'abcde1%' order by c1;
select c1 from t1 where c1 like 'abcde11%' order by c1;
select c1 from t1 where c1 like 'abcde111%' order by c1;
drop table t1;
