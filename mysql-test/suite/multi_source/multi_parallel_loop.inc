#create a table,insert data and drop table

#parameters
#  loop_counter
#  table_engine
--echo ##Running CURD operation
--disable_query_log
while ($loop_counter)
{
  #DDL statement
  --eval create table t1(a int primary key) engine=$table_engine;

  #non trans update statement
  insert into t1 values(1);
  insert into t1 values(2);
  drop table t1;
  --dec $loop_counter
}
--enable_query_log
