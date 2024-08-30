#
# Create all tables, sequences and views needed for drop combinations;
#

if ($create_option != 'temporary')
{
  show tables;
}

--echo # Generating test tables
--disable_query_log
--disable_warnings
eval create $create_option table if not exists t1 (a int);
eval create $create_option table if not exists t2 (a int);
eval create $create_option sequence if not exists s1;
eval create $create_option sequence if not exists s2;
if ($create_option != 'temporary')
{
  eval create view if not exists v1 as SELECT * from t1;
  eval create view if not exists v2 as SELECT * from t2;
}

--enable_warnings
--enable_query_log
